# frozen_string_literal: true

require_relative 'test_helper'

class CacheContractTest < CacheApiTestCase
  # CT-001
  def test_openapi_document_loads_and_has_required_top_level_keys
    doc = openapi_document

    assert_kind_of Hash, doc
    assert_equal '3.1.0', doc['openapi']
    assert doc.key?('paths'), 'missing paths section'
    assert doc.key?('components'), 'missing components section'
  end

  # CT-002
  def test_operation_ids_are_unique
    doc = openapi_document
    operations = []

    doc.fetch('paths').each_value do |path_item|
      next unless path_item.is_a?(Hash)

      %w[get post put patch delete options head].each do |method|
        op = path_item[method]
        next unless op.is_a?(Hash)

        operations << op['operationId'] if op['operationId']
      end
    end

    duplicates = operations.group_by(&:itself).select { |_k, v| v.length > 1 }.keys
    assert duplicates.empty?, "duplicate operationIds: #{duplicates.inspect}"
  end

  # CT-003
  def test_error_schema_requires_code_and_message
    doc = openapi_document
    err = doc.dig('components', 'schemas', 'Error')

    assert_kind_of Hash, err
    required = err.fetch('required')
    assert_includes required, 'code'
    assert_includes required, 'message'
  end

  def test_required_paths_are_present
    doc = openapi_document
    paths = doc.fetch('paths').keys

    required = [
      '/v1/instances',
      '/v1/instances/{id}',
      '/v1/instances/{id}/register',
      '/v1/instances/{id}/heartbeat',
      '/v1/instances/{id}/deregister',
      '/v1/content/locate/{key}',
      '/v1/content/nearest/{key}',
      '/v1/content/instances/{id}/keys',
      '/v1/topology',
      '/v1/topology/proximity',
      '/v1/topology/routes/{from}/{to}',
      '/v1/events',
      '/v1/events/batch'
    ]

    required.each { |p| assert_includes paths, p }
  end

  def test_schema_constraints_required_by_tests_spec
    doc = openapi_document

    id_pattern = doc.dig('components', 'parameters', 'InstanceId', 'schema', 'pattern')
    assert_equal '^[a-zA-Z0-9_-]+$', id_pattern

    status_enum = doc.dig('components', 'schemas', 'CacheInstance', 'properties', 'status', 'enum')
    assert_equal %w[active inactive unverified], status_enum

    tier_enum = doc.dig('components', 'schemas', 'RegisterInstanceRequest', 'properties', 'tier', 'enum')
    assert_equal %w[root branch leaf], tier_enum

    event_enum = doc.dig('components', 'schemas', 'CacheEvent', 'properties', 'eventType', 'enum')
    assert_equal %w[key_added key_updated key_evicted], event_enum

    nearest_params = doc.dig('paths', '/v1/content/nearest/{key}', 'get', 'parameters') || []
    src_param = nearest_params.find { |p| p['name'] == 'sourceInstanceId' }
    refute_nil src_param
    assert_equal true, src_param['required']

    batch_events = doc.dig('components', 'schemas', 'CacheEventBatch', 'properties', 'events')
    assert_equal 1, batch_events['minItems']
    assert_equal 10_000, batch_events['maxItems']
  end
end
