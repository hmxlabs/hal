# TOP-001, TOP-002, TOP-003, TOP-004, TOP-005

POST {{base_url}}/v1/instances/{{test_prefix}}_top_root/register
Content-Type: application/json
{
  "address": "127.0.0.1:18379",
  "region": "us-west",
  "tier": "root",
  "maxSize": 1073741824
}
HTTP 201

POST {{base_url}}/v1/instances/{{test_prefix}}_top_branch/register
Content-Type: application/json
{
  "address": "127.0.0.1:18380",
  "region": "us-west",
  "tier": "branch",
  "parentId": "{{test_prefix}}_top_root",
  "maxSize": 536870912
}
HTTP 201

POST {{base_url}}/v1/instances/{{test_prefix}}_top_leaf/register
Content-Type: application/json
{
  "address": "127.0.0.1:18381",
  "region": "us-west",
  "tier": "leaf",
  "parentId": "{{test_prefix}}_top_branch",
  "maxSize": 268435456
}
HTTP 201

GET {{base_url}}/v1/topology
HTTP 200
[Asserts]
jsonpath "$.nodes" exists
jsonpath "$.edges" exists

GET {{base_url}}/v1/topology/proximity
HTTP 200
[Asserts]
jsonpath "$.instances" exists
jsonpath "$.matrix" exists

GET {{base_url}}/v1/topology/proximity?instanceIds={{test_prefix}}_top_root,{{test_prefix}}_top_leaf
HTTP 200
[Asserts]
jsonpath "$.instances" exists
jsonpath "$.matrix" exists

GET {{base_url}}/v1/topology/routes/{{test_prefix}}_top_root/{{test_prefix}}_top_leaf
HTTP 200
[Asserts]
jsonpath "$.from" == "{{test_prefix}}_top_root"
jsonpath "$.to" == "{{test_prefix}}_top_leaf"
jsonpath "$.hops" exists

GET {{base_url}}/v1/topology/routes/{{test_prefix}}_top_missing/{{test_prefix}}_top_leaf
HTTP 404
[Asserts]
jsonpath "$.code" exists
jsonpath "$.message" exists

DELETE {{base_url}}/v1/instances/{{test_prefix}}_top_leaf/deregister
HTTP 204

DELETE {{base_url}}/v1/instances/{{test_prefix}}_top_branch/deregister
HTTP 204

DELETE {{base_url}}/v1/instances/{{test_prefix}}_top_root/deregister
HTTP 204
