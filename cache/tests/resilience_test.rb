# frozen_string_literal: true

require_relative 'test_helper'

class CacheResilienceTest < CacheApiTestCase
  # RES-001
  def test_control_plane_restart_preserves_registry_and_topology
    require_api!

    root, branch, leaf = seed_canonical_hierarchy(prefix: unique_id('res1'))
    @created_instance_ids.concat([root, branch, leaf])

    run_restart_command!

    recovered = wait_until(timeout_s: Integer(ENV.fetch('CACHE_RECOVERY_TIMEOUT', '60')), interval_s: 2) do
      code, = request_json(:get, '/v1/instances')
      code == 200
    rescue StandardError
      false
    end
    assert recovered, 'control plane did not recover within timeout'

    topo_code, topo_json, = request_json(:get, '/v1/topology')
    assert_equal 200, topo_code
    node_ids = topo_json.fetch('nodes').map { |n| n['id'] }

    assert_includes node_ids, root
    assert_includes node_ids, branch
    assert_includes node_ids, leaf
  end

  # RES-002
  def test_reconciliation_after_restart_with_inventory_resubmission
    require_api!

    root, _branch, leaf = seed_canonical_hierarchy(prefix: unique_id('res2'))
    @created_instance_ids.concat([root, _branch, leaf])

    key = 'dataset:imagenet:v1'
    first_code, = request_json(:post, "/v1/content/instances/#{root}/keys", body: { mode: 'partial', keys: [key_info(key, 5000)] })
    assert_equal 200, first_code

    run_restart_command!

    recovered = wait_until(timeout_s: Integer(ENV.fetch('CACHE_RECOVERY_TIMEOUT', '60')), interval_s: 2) do
      code, = request_json(:get, '/v1/instances')
      code == 200
    rescue StandardError
      false
    end
    assert recovered, 'control plane did not recover within timeout'

    resubmit_code, = request_json(:post, "/v1/content/instances/#{root}/keys", body: { mode: 'partial', keys: [key_info(key, 5000)] })
    assert_equal 200, resubmit_code

    locate_code, locate_json, = request_json(:get, "/v1/content/nearest/#{URI.encode_www_form_component(key)}", query: { sourceInstanceId: leaf })
    assert_equal 200, locate_code
    assert_equal key, locate_json['key']
  end
end
