# frozen_string_literal: true

require 'json'
require 'minitest/autorun'
require 'net/http'
require 'openssl'
require 'securerandom'
require 'time'
require 'uri'
require 'yaml'

module CacheTestSupport
  OPENAPI_PATH = File.expand_path('../openapi.yaml', __dir__)

  def openapi_document
    return @openapi_document if defined?(@openapi_document)

    raw = File.read(OPENAPI_PATH)
    @openapi_document = YAML.safe_load(raw, aliases: true)
  end

  def api_base_url
    ENV['CACHE_API_BASE_URL']
  end

  def api_configured?
    api_base_url && !api_base_url.strip.empty?
  end

  def require_api!
    skip 'Set CACHE_API_BASE_URL to run API-backed tests' unless api_configured?
  end

  def build_uri(path, query = nil)
    base = api_base_url
    raise 'CACHE_API_BASE_URL is not set' unless api_configured?

    normalized_base = base.end_with?('/') ? base : "#{base}/"
    normalized_path = path.sub(%r{^/}, '')
    uri = URI.join(normalized_base, normalized_path)
    uri.query = URI.encode_www_form(query) if query && !query.empty?
    uri
  end

  def request_json(method, path, query: nil, body: nil, headers: {})
    uri = build_uri(path, query)
    http = Net::HTTP.new(uri.host, uri.port)
    http.open_timeout = Integer(ENV.fetch('CACHE_HTTP_OPEN_TIMEOUT', '5'))
    http.read_timeout = Integer(ENV.fetch('CACHE_HTTP_READ_TIMEOUT', '20'))

    if uri.scheme == 'https'
      http.use_ssl = true
      http.verify_mode = ENV['CACHE_API_INSECURE'] == '1' ? OpenSSL::SSL::VERIFY_NONE : OpenSSL::SSL::VERIFY_PEER
    end

    req_class = case method.to_s.downcase
                when 'get' then Net::HTTP::Get
                when 'post' then Net::HTTP::Post
                when 'delete' then Net::HTTP::Delete
                else
                  raise ArgumentError, "Unsupported HTTP method: #{method}"
                end

    request = req_class.new(uri)
    request['Accept'] = 'application/json'
    headers.each { |k, v| request[k] = v }

    if body
      request['Content-Type'] = 'application/json'
      request.body = JSON.generate(body)
    end

    response = http.request(request)
    parsed = parse_json(response.body)
    [response.code.to_i, parsed, response.body, response]
  end

  def parse_json(raw)
    return nil if raw.nil? || raw.strip.empty?

    JSON.parse(raw)
  rescue JSON::ParserError
    nil
  end

  def assert_error_schema(json)
    assert_kind_of Hash, json, 'expected error payload as JSON object'
    assert json.key?('code'), 'expected error.code'
    assert json.key?('message'), 'expected error.message'
  end

  def unique_id(prefix)
    stamp = Time.now.utc.strftime('%Y%m%d%H%M%S')
    rand = SecureRandom.hex(3)
    "#{prefix}_#{stamp}_#{rand}"
  end

  def iso_now
    Time.now.utc.iso8601
  end

  def register_instance(id:, tier:, parent_id: nil, region: 'test-region')
    payload = {
      address: "127.0.0.1:#{rand(10_000..65_000)}",
      region: region,
      tier: tier,
      maxSize: 1_073_741_824
    }
    payload[:parentId] = parent_id if parent_id

    request_json(:post, "/v1/instances/#{id}/register", body: payload)
  end

  def deregister_instance(id)
    request_json(:delete, "/v1/instances/#{id}/deregister")
  end

  def safe_deregister(id)
    code, = deregister_instance(id)
    [204, 404].include?(code)
  rescue StandardError
    false
  end

  def seed_canonical_hierarchy(prefix: unique_id('cachetest'))
    root_id = "#{prefix}_root"
    branch_id = "#{prefix}_branch"
    leaf_id = "#{prefix}_leaf"

    root_code, root_json = register_instance(id: root_id, tier: 'root')
    assert_includes [201, 409], root_code, "register root failed: #{root_json.inspect}"

    branch_code, branch_json = register_instance(id: branch_id, tier: 'branch', parent_id: root_id)
    assert_includes [201, 409], branch_code, "register branch failed: #{branch_json.inspect}"

    leaf_code, leaf_json = register_instance(id: leaf_id, tier: 'leaf', parent_id: branch_id)
    assert_includes [201, 409], leaf_code, "register leaf failed: #{leaf_json.inspect}"

    [root_id, branch_id, leaf_id]
  end

  def key_info(key, size)
    { key: key, size: size, lastAccessed: iso_now }
  end

  def p95(values)
    return 0.0 if values.empty?

    sorted = values.sort
    idx = (0.95 * (sorted.length - 1)).ceil
    sorted[idx]
  end

  def assert_accepted_error_status(code)
    assert_includes [400, 404], code, "expected validation/not-found status, got #{code}"
  end

  def run_restart_command!
    cmd = ENV['CACHE_API_RESTART_CMD']
    skip 'Set CACHE_API_RESTART_CMD to run resilience restart tests' if cmd.nil? || cmd.strip.empty?

    ok = system(cmd)
    assert ok, "restart command failed: #{cmd}"
  end

  def wait_until(timeout_s: 30, interval_s: 1)
    deadline = Time.now + timeout_s
    loop do
      return true if yield
      return false if Time.now >= deadline

      sleep interval_s
    end
  end
end

class CacheApiTestCase < Minitest::Test
  include CacheTestSupport

  def setup
    @created_instance_ids = []
  end

  def teardown
    return unless api_configured?

    @created_instance_ids.reverse_each { |id| safe_deregister(id) }
  end

  def track_instance(id)
    @created_instance_ids << id
    id
  end
end
