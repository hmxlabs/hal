# frozen_string_literal: true

require_relative 'test_helper'

class CacheIntegrationTest < CacheApiTestCase
  # INT-001
  def test_lifecycle_workflow_register_heartbeat_keys_lookup_events_deregister
    require_api!

    root, branch, leaf = seed_canonical_hierarchy(prefix: unique_id('int1'))
    @created_instance_ids.concat([root, branch, leaf])

    hb_code, hb_json, = request_json(:post, "/v1/instances/#{leaf}/heartbeat", body: { healthy: true, keyCount: 0 })
    assert_equal 200, hb_code
    assert_equal true, hb_json['acknowledged']

    key = 'dataset:imagenet:v1'
    update_code, = request_json(:post, "/v1/content/instances/#{root}/keys", body: { mode: 'partial', keys: [key_info(key, 4096)] })
    assert_equal 200, update_code

    nearest_code, nearest_json, = request_json(:get, "/v1/content/nearest/#{URI.encode_www_form_component(key)}", query: { sourceInstanceId: leaf })
    assert_equal 200, nearest_code
    assert nearest_json.key?('instanceId')

    event_payload = {
      instanceId: leaf,
      eventType: 'key_added',
      timestamp: iso_now,
      key: key,
      size: 4096,
      sourceInstanceId: root,
      retrievalTimeMs: 15.1
    }
    event_code, = request_json(:post, '/v1/events', body: event_payload)
    assert_equal 202, event_code

    dereg_code, = request_json(:delete, "/v1/instances/#{leaf}/deregister")
    assert_equal 204, dereg_code
  end

  # INT-002
  def test_event_and_inventory_updates_are_reflected_in_content_lookup
    require_api!

    root, _branch, leaf = seed_canonical_hierarchy(prefix: unique_id('int2'))
    @created_instance_ids.concat([root, _branch, leaf])

    key = 'checkpoint:resnet50:epoch10'
    root_update_code, = request_json(:post, "/v1/content/instances/#{root}/keys", body: { mode: 'partial', keys: [key_info(key, 8192)] })
    assert_equal 200, root_update_code

    before_code, before_json, = request_json(:get, "/v1/content/locate/#{URI.encode_www_form_component(key)}")
    assert_equal 200, before_code
    holders_before = before_json.fetch('instances').map { |h| h['instanceId'] }
    assert_includes holders_before, root

    leaf_update_code, = request_json(:post, "/v1/content/instances/#{leaf}/keys", body: { mode: 'partial', keys: [key_info(key, 8192)] })
    assert_equal 200, leaf_update_code

    event_code, = request_json(:post, '/v1/events', body: {
      instanceId: leaf,
      eventType: 'key_added',
      timestamp: iso_now,
      key: key,
      size: 8192,
      sourceInstanceId: root,
      retrievalTimeMs: 3.3
    })
    assert_equal 202, event_code

    after_code, after_json, = request_json(:get, "/v1/content/locate/#{URI.encode_www_form_component(key)}")
    assert_equal 200, after_code
    holders_after = after_json.fetch('instances').map { |h| h['instanceId'] }
    assert_includes holders_after, leaf
  end

  # INT-003
  def test_deregistered_instance_is_removed_from_key_availability
    require_api!

    root, _branch, leaf = seed_canonical_hierarchy(prefix: unique_id('int3'))
    @created_instance_ids.concat([root, _branch, leaf])

    key = 'dataset:llama2-tokenizer'
    update_code, = request_json(:post, "/v1/content/instances/#{leaf}/keys", body: { mode: 'partial', keys: [key_info(key, 1024)] })
    assert_equal 200, update_code

    before_code, before_json, = request_json(:get, "/v1/content/locate/#{URI.encode_www_form_component(key)}")
    assert_equal 200, before_code
    assert_includes before_json.fetch('instances').map { |h| h['instanceId'] }, leaf

    del_code, = request_json(:delete, "/v1/instances/#{leaf}/deregister")
    assert_equal 204, del_code

    after_code, after_json, = request_json(:get, "/v1/content/locate/#{URI.encode_www_form_component(key)}")
    assert_equal 200, after_code
    refute_includes after_json.fetch('instances').map { |h| h['instanceId'] }, leaf
  end

  # INT-004
  def test_topology_consistency_after_registration
    require_api!

    root, branch, leaf = seed_canonical_hierarchy(prefix: unique_id('int4'))
    @created_instance_ids.concat([root, branch, leaf])

    topo_code, topo_json, = request_json(:get, '/v1/topology')
    assert_equal 200, topo_code

    node_ids = topo_json.fetch('nodes').map { |n| n['id'] }
    assert_includes node_ids, root
    assert_includes node_ids, branch
    assert_includes node_ids, leaf

    route_code, route_json, = request_json(:get, "/v1/topology/routes/#{root}/#{leaf}")
    assert_equal 200, route_code
    assert_operator route_json.fetch('hops').length, :>=, 2
  end
end
