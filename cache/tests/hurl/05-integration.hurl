# INT-001, INT-002, INT-004

POST {{base_url}}/v1/instances/{{test_prefix}}_int_root/register
Content-Type: application/json
{
  "address": "127.0.0.1:20379",
  "region": "us-west",
  "tier": "root",
  "maxSize": 1073741824
}
HTTP 201

POST {{base_url}}/v1/instances/{{test_prefix}}_int_branch/register
Content-Type: application/json
{
  "address": "127.0.0.1:20380",
  "region": "us-west",
  "tier": "branch",
  "parentId": "{{test_prefix}}_int_root",
  "maxSize": 536870912
}
HTTP 201

POST {{base_url}}/v1/instances/{{test_prefix}}_int_leaf/register
Content-Type: application/json
{
  "address": "127.0.0.1:20381",
  "region": "us-west",
  "tier": "leaf",
  "parentId": "{{test_prefix}}_int_branch",
  "maxSize": 268435456
}
HTTP 201

POST {{base_url}}/v1/instances/{{test_prefix}}_int_leaf/heartbeat
Content-Type: application/json
{
  "healthy": true,
  "keyCount": 0,
  "totalSize": 0,
  "hitRate": 0.0
}
HTTP 200
[Asserts]
jsonpath "$.acknowledged" == true

POST {{base_url}}/v1/content/instances/{{test_prefix}}_int_root/keys
Content-Type: application/json
{
  "mode": "partial",
  "keys": [
    {
      "key": "dataset_int_test",
      "size": 4096,
      "lastAccessed": "2026-01-01T00:00:00Z"
    }
  ]
}
HTTP 200

GET {{base_url}}/v1/content/nearest/dataset_int_test?sourceInstanceId={{test_prefix}}_int_leaf
HTTP 200
[Asserts]
jsonpath "$.instanceId" exists

POST {{base_url}}/v1/events
Content-Type: application/json
{
  "instanceId": "{{test_prefix}}_int_leaf",
  "eventType": "key_added",
  "timestamp": "2026-01-01T00:00:00Z",
  "key": "dataset_int_test",
  "size": 4096,
  "sourceInstanceId": "{{test_prefix}}_int_root",
  "retrievalTimeMs": 7.1
}
HTTP 202

POST {{base_url}}/v1/content/instances/{{test_prefix}}_int_leaf/keys
Content-Type: application/json
{
  "mode": "partial",
  "keys": [
    {
      "key": "dataset_int_test",
      "size": 4096,
      "lastAccessed": "2026-01-01T00:00:00Z"
    }
  ]
}
HTTP 200

GET {{base_url}}/v1/content/locate/dataset_int_test
HTTP 200
[Asserts]
jsonpath "$.instances" exists

GET {{base_url}}/v1/topology
HTTP 200
[Asserts]
jsonpath "$.nodes" exists
jsonpath "$.edges" exists

GET {{base_url}}/v1/topology/routes/{{test_prefix}}_int_root/{{test_prefix}}_int_leaf
HTTP 200
[Asserts]
jsonpath "$.hops" exists

DELETE {{base_url}}/v1/instances/{{test_prefix}}_int_leaf/deregister
HTTP 204

DELETE {{base_url}}/v1/instances/{{test_prefix}}_int_branch/deregister
HTTP 204

DELETE {{base_url}}/v1/instances/{{test_prefix}}_int_root/deregister
HTTP 204
