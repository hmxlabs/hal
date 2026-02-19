# frozen_string_literal: true

require_relative 'test_helper'

class CachePerformanceTest < CacheApiTestCase
  def setup
    super
    skip 'Set CACHE_ENABLE_PERF=1 to run performance tests' unless ENV['CACHE_ENABLE_PERF'] == '1'
    require_api!
  end

  # PERF-001
  def test_batch_event_ingestion_throughput
    instance = track_instance(unique_id('perf1'))
    reg_code, = register_instance(id: instance, tier: 'root')
    assert_equal 201, reg_code

    batch_size = Integer(ENV.fetch('CACHE_PERF_BATCH_SIZE', '100'))
    rounds = Integer(ENV.fetch('CACHE_PERF_ROUNDS', '20'))
    min_eps = Float(ENV.fetch('CACHE_PERF_MIN_EVENTS_PER_SEC', '100.0'))

    payload_template = {
      instanceId: instance,
      events: []
    }

    start = Process.clock_gettime(Process::CLOCK_MONOTONIC)
    rounds.times do |r|
      events = Array.new(batch_size) do |i|
        {
          eventType: 'key_added',
          timestamp: iso_now,
          key: "perf:key:#{r}:#{i}",
          size: i + 1
        }
      end

      payload = payload_template.merge(events: events)
      code, json, = request_json(:post, '/v1/events/batch', body: payload)
      assert_equal 202, code
      assert_equal batch_size, json['count']
    end
    elapsed = Process.clock_gettime(Process::CLOCK_MONOTONIC) - start

    total_events = batch_size * rounds
    eps = total_events / elapsed
    assert_operator eps, :>=, min_eps, format('expected >= %.2f eps, got %.2f eps', min_eps, eps)
  end

  # PERF-002
  def test_nearest_lookup_p95_latency
    root, _branch, leaf = seed_canonical_hierarchy(prefix: unique_id('perf2'))
    @created_instance_ids.concat([root, _branch, leaf])

    key = 'dataset:imagenet:v1'
    update_code, = request_json(:post, "/v1/content/instances/#{root}/keys", body: { mode: 'partial', keys: [key_info(key, 2048)] })
    assert_equal 200, update_code

    iterations = Integer(ENV.fetch('CACHE_PERF_LOOKUP_ITERATIONS', '100'))
    max_p95_ms = Float(ENV.fetch('CACHE_PERF_NEAREST_P95_MS', '200.0'))

    samples = []
    iterations.times do
      t0 = Process.clock_gettime(Process::CLOCK_MONOTONIC)
      code, = request_json(:get, "/v1/content/nearest/#{URI.encode_www_form_component(key)}", query: { sourceInstanceId: leaf })
      t1 = Process.clock_gettime(Process::CLOCK_MONOTONIC)

      assert_equal 200, code
      samples << ((t1 - t0) * 1000.0)
    end

    latency_p95 = p95(samples)
    assert_operator latency_p95, :<=, max_p95_ms,
                    format('expected p95 <= %.2fms, got %.2fms', max_p95_ms, latency_p95)
  end

  # PERF-003
  def test_route_lookup_p95_latency
    root, _branch, leaf = seed_canonical_hierarchy(prefix: unique_id('perf3'))
    @created_instance_ids.concat([root, _branch, leaf])

    iterations = Integer(ENV.fetch('CACHE_PERF_ROUTE_ITERATIONS', '100'))
    max_p95_ms = Float(ENV.fetch('CACHE_PERF_ROUTE_P95_MS', '250.0'))

    samples = []
    iterations.times do
      t0 = Process.clock_gettime(Process::CLOCK_MONOTONIC)
      code, = request_json(:get, "/v1/topology/routes/#{root}/#{leaf}")
      t1 = Process.clock_gettime(Process::CLOCK_MONOTONIC)

      assert_equal 200, code
      samples << ((t1 - t0) * 1000.0)
    end

    latency_p95 = p95(samples)
    assert_operator latency_p95, :<=, max_p95_ms,
                    format('expected p95 <= %.2fms, got %.2fms', max_p95_ms, latency_p95)
  end
end
