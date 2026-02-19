# RES-001, RES-002 verify

GET {{base_url}}/v1/instances/{{test_prefix}}_res_root
HTTP 200
[Asserts]
jsonpath "$.id" == "{{test_prefix}}_res_root"

GET {{base_url}}/v1/instances/{{test_prefix}}_res_branch
HTTP 200

GET {{base_url}}/v1/instances/{{test_prefix}}_res_leaf
HTTP 200

GET {{base_url}}/v1/topology
HTTP 200
[Asserts]
jsonpath "$.nodes" exists
jsonpath "$.edges" exists

POST {{base_url}}/v1/content/instances/{{test_prefix}}_res_root/keys
Content-Type: application/json
{
  "mode": "partial",
  "keys": [
    {
      "key": "dataset_resilience_test",
      "size": 5000,
      "lastAccessed": "2026-01-01T00:00:00Z"
    }
  ]
}
HTTP 200

GET {{base_url}}/v1/content/nearest/dataset_resilience_test?sourceInstanceId={{test_prefix}}_res_leaf
HTTP 200
[Asserts]
jsonpath "$.instanceId" exists

DELETE {{base_url}}/v1/instances/{{test_prefix}}_res_leaf/deregister
HTTP 204

DELETE {{base_url}}/v1/instances/{{test_prefix}}_res_branch/deregister
HTTP 204

DELETE {{base_url}}/v1/instances/{{test_prefix}}_res_root/deregister
HTTP 204
