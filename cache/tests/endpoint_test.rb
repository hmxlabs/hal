# frozen_string_literal: true

require_relative 'test_helper'

class CacheEndpointTest < CacheApiTestCase
  # INS-001, INS-003, INS-005, INS-010, INS-012
  def test_instance_lifecycle_happy_path_and_duplicate_registration
    require_api!

    id = track_instance(unique_id('inst'))

    code, json, = register_instance(id: id, tier: 'root')
    assert_equal 201, code
    assert_equal id, json['id'] if json.is_a?(Hash) && json.key?('id')

    dup_code, dup_json, = register_instance(id: id, tier: 'root')
    assert_equal 409, dup_code
    assert_error_schema(dup_json)

    get_code, get_json, = request_json(:get, "/v1/instances/#{id}")
    assert_equal 200, get_code
    assert_equal id, get_json['id']

    hb_payload = { keyCount: 1, totalSize: 1024, hitRate: 0.5, healthy: true }
    hb_code, hb_json, = request_json(:post, "/v1/instances/#{id}/heartbeat", body: hb_payload)
    assert_equal 200, hb_code
    assert_equal true, hb_json['acknowledged']

    del_code, = request_json(:delete, "/v1/instances/#{id}/deregister")
    assert_equal 204, del_code
  end

  # INS-002, INS-004
  def test_register_branch_with_parent_and_reject_invalid_tier
    require_api!

    root = track_instance(unique_id('root'))
    root_code, = register_instance(id: root, tier: 'root')
    assert_equal 201, root_code

    branch = track_instance(unique_id('branch'))
    branch_code, = register_instance(id: branch, tier: 'branch', parent_id: root)
    assert_equal 201, branch_code

    bad = unique_id('badtier')
    bad_payload = {
      address: '127.0.0.1:12345',
      region: 'test-region',
      tier: 'edge'
    }
    bad_code, bad_json, = request_json(:post, "/v1/instances/#{bad}/register", body: bad_payload)
    assert_equal 400, bad_code
    assert_error_schema(bad_json)
  end

  # INS-006, INS-011, INS-013
  def test_not_found_instance_endpoints
    require_api!

    missing = unique_id('missing')

    get_code, get_json, = request_json(:get, "/v1/instances/#{missing}")
    assert_equal 404, get_code
    assert_error_schema(get_json)

    hb_code, hb_json, = request_json(:post, "/v1/instances/#{missing}/heartbeat", body: { healthy: true })
    assert_equal 404, hb_code
    assert_error_schema(hb_json)

    del_code, del_json, = request_json(:delete, "/v1/instances/#{missing}/deregister")
    assert_equal 404, del_code
    assert_error_schema(del_json)
  end

  # INS-007, INS-008, INS-009
  def test_list_instances_and_filters
    require_api!

    id = track_instance(unique_id('listing'))
    register_code, = register_instance(id: id, tier: 'root', region: 'us-west')
    assert_equal 201, register_code

    code, json, = request_json(:get, '/v1/instances')
    assert_equal 200, code
    assert_kind_of Hash, json
    assert_kind_of Array, json['instances']

    status_code, status_json, = request_json(:get, '/v1/instances', query: { status: 'active' })
    assert_equal 200, status_code
    assert_kind_of Array, status_json['instances']

    region_code, region_json, = request_json(:get, '/v1/instances', query: { region: 'us-west' })
    assert_equal 200, region_code
    assert_kind_of Array, region_json['instances']
  end

  # VAL-001
  def test_invalid_instance_id_pattern_is_rejected
    require_api!

    code, json, = request_json(:get, '/v1/instances/bad$id')
    assert_accepted_error_status(code)
    assert_error_schema(json) if json
  end

  # CNT-001, CNT-002, CNT-003, CNT-004
  def test_key_inventory_update_and_listing_modes
    require_api!

    instance = track_instance(unique_id('keys'))
    reg_code, = register_instance(id: instance, tier: 'root')
    assert_equal 201, reg_code

    partial_body = {
      mode: 'partial',
      keys: [
        key_info('dataset:imagenet:v1', 1000),
        key_info('dataset:llama2-tokenizer', 2000)
      ]
    }
    p_code, p_json, = request_json(:post, "/v1/content/instances/#{instance}/keys", body: partial_body)
    assert_equal 200, p_code
    assert_operator p_json['updated'], :>=, 2

    list_code, list_json, = request_json(:get, "/v1/content/instances/#{instance}/keys")
    assert_equal 200, list_code
    assert_equal instance, list_json['instanceId']
    assert_kind_of Array, list_json['keys']

    paged_code, paged_json, = request_json(:get, "/v1/content/instances/#{instance}/keys", query: { limit: 1 })
    assert_equal 200, paged_code
    assert_operator paged_json['keys'].size, :<=, 1

    full_body = {
      mode: 'full',
      keys: [key_info('checkpoint:resnet50:epoch10', 3000)]
    }
    f_code, f_json, = request_json(:post, "/v1/content/instances/#{instance}/keys", body: full_body)
    assert_equal 200, f_code
    assert_operator f_json['removed'], :>=, 1
  end

  # CNT-005, CNT-006, CNT-007, CNT-008, CNT-009
  def test_content_lookup_locate_and_nearest
    require_api!

    root, branch, leaf = seed_canonical_hierarchy(prefix: unique_id('lookup'))
    @created_instance_ids.concat([root, branch, leaf])

    key = 'dataset:imagenet:v1'
    root_update = { mode: 'partial', keys: [key_info(key, 1234)] }
    r_code, = request_json(:post, "/v1/content/instances/#{root}/keys", body: root_update)
    assert_equal 200, r_code

    branch_update = { mode: 'partial', keys: [key_info(key, 1234)] }
    b_code, = request_json(:post, "/v1/content/instances/#{branch}/keys", body: branch_update)
    assert_equal 200, b_code

    locate_code, locate_json, = request_json(:get, "/v1/content/locate/#{URI.encode_www_form_component(key)}")
    assert_equal 200, locate_code
    assert_equal key, locate_json['key']
    assert_kind_of Array, locate_json['instances']
    refute_empty locate_json['instances']

    ordered_code, ordered_json, = request_json(:get, "/v1/content/locate/#{URI.encode_www_form_component(key)}", query: { sourceInstanceId: leaf })
    assert_equal 200, ordered_code
    assert_kind_of Array, ordered_json['instances']

    nearest_code, nearest_json, = request_json(:get, "/v1/content/nearest/#{URI.encode_www_form_component(key)}", query: { sourceInstanceId: leaf })
    assert_equal 200, nearest_code
    assert_equal key, nearest_json['key']
    assert nearest_json.key?('instanceId')

    missing_q_code, missing_q_json, = request_json(:get, "/v1/content/nearest/#{URI.encode_www_form_component(key)}")
    assert_equal 400, missing_q_code
    assert_error_schema(missing_q_json)

    missing_k_code, missing_k_json, = request_json(:get, "/v1/content/nearest/#{URI.encode_www_form_component('missing:key')}", query: { sourceInstanceId: leaf })
    assert_equal 404, missing_k_code
    assert_error_schema(missing_k_json)
  end

  # TOP-001, TOP-002, TOP-003, TOP-004, TOP-005
  def test_topology_endpoints
    require_api!

    root, _branch, leaf = seed_canonical_hierarchy(prefix: unique_id('topo'))
    @created_instance_ids.concat([root, _branch, leaf])

    topo_code, topo_json, = request_json(:get, '/v1/topology')
    assert_equal 200, topo_code
    assert_kind_of Array, topo_json['nodes']
    assert_kind_of Array, topo_json['edges']

    prox_code, prox_json, = request_json(:get, '/v1/topology/proximity')
    assert_equal 200, prox_code
    assert_kind_of Array, prox_json['instances']
    assert_kind_of Array, prox_json['matrix']

    filter_code, filter_json, = request_json(:get, '/v1/topology/proximity', query: { instanceIds: "#{root},#{leaf}" })
    assert_equal 200, filter_code
    assert_kind_of Array, filter_json['instances']

    route_code, route_json, = request_json(:get, "/v1/topology/routes/#{root}/#{leaf}")
    assert_equal 200, route_code
    assert_kind_of Array, route_json['hops']

    miss_code, miss_json, = request_json(:get, "/v1/topology/routes/#{unique_id('none')}/#{leaf}")
    assert_equal 404, miss_code
    assert_error_schema(miss_json)
  end

  # EVT-001, EVT-002, EVT-003, EVT-004, EVT-005, EVT-006
  def test_events_ingestion_single_and_batch
    require_api!

    instance = track_instance(unique_id('events'))
    reg_code, = register_instance(id: instance, tier: 'root')
    assert_equal 201, reg_code

    single_payload = {
      instanceId: instance,
      eventType: 'key_added',
      timestamp: iso_now,
      key: 'dataset:imagenet:v1',
      size: 100,
      sourceInstanceId: instance,
      retrievalTimeMs: 1.2
    }
    s_code, s_json, = request_json(:post, '/v1/events', body: single_payload)
    assert_equal 202, s_code
    assert_equal true, s_json['accepted']
    assert s_json.key?('eventId')

    invalid_payload = single_payload.merge(eventType: 'key_removed')
    i_code, i_json, = request_json(:post, '/v1/events', body: invalid_payload)
    assert_equal 400, i_code
    assert_error_schema(i_json)

    batch_payload = {
      instanceId: instance,
      events: [
        { eventType: 'key_added', timestamp: iso_now, key: 'k1', size: 1 },
        { eventType: 'key_updated', timestamp: iso_now, key: 'k2', size: 2 }
      ]
    }
    b_code, b_json, = request_json(:post, '/v1/events/batch', body: batch_payload)
    assert_equal 202, b_code
    assert_equal true, b_json['accepted']
    assert_equal 2, b_json['count']

    empty_code, empty_json, = request_json(:post, '/v1/events/batch', body: { instanceId: instance, events: [] })
    assert_equal 400, empty_code
    assert_error_schema(empty_json)

    too_many_events = Array.new(10_001) { { eventType: 'key_added', timestamp: iso_now, key: "k#{rand(10_000_000)}" } }
    over_code, over_json, = request_json(:post, '/v1/events/batch', body: { instanceId: instance, events: too_many_events })
    assert_equal 400, over_code
    assert_error_schema(over_json)

    mixed_payload = {
      instanceId: instance,
      events: [
        { eventType: 'key_added', timestamp: iso_now, key: 'mk1', size: 10 },
        { eventType: 'key_updated', timestamp: iso_now, key: 'mk2', size: 20 },
        { eventType: 'key_evicted', timestamp: iso_now, key: 'mk3' }
      ]
    }
    m_code, m_json, = request_json(:post, '/v1/events/batch', body: mixed_payload)
    assert_equal 202, m_code
    assert_equal 3, m_json['count']
  end
end
