# INS-001, INS-002, INS-003, INS-004, INS-005, INS-006, INS-007, INS-008, INS-009, INS-010, INS-011, INS-012, INS-013

POST {{base_url}}/v1/instances/{{test_prefix}}_ins_root/register
Content-Type: application/json
{
  "address": "127.0.0.1:16379",
  "region": "us-west",
  "tier": "root",
  "maxSize": 1073741824
}
HTTP 201
[Asserts]
jsonpath "$.id" == "{{test_prefix}}_ins_root"
jsonpath "$.registered" == true

POST {{base_url}}/v1/instances/{{test_prefix}}_ins_root/register
Content-Type: application/json
{
  "address": "127.0.0.1:16379",
  "region": "us-west",
  "tier": "root",
  "maxSize": 1073741824
}
HTTP 409
[Asserts]
jsonpath "$.code" exists
jsonpath "$.message" exists

POST {{base_url}}/v1/instances/{{test_prefix}}_ins_branch/register
Content-Type: application/json
{
  "address": "127.0.0.1:16380",
  "region": "us-west",
  "tier": "branch",
  "parentId": "{{test_prefix}}_ins_root",
  "maxSize": 536870912
}
HTTP 201
[Asserts]
jsonpath "$.id" == "{{test_prefix}}_ins_branch"
jsonpath "$.registered" == true

POST {{base_url}}/v1/instances/{{test_prefix}}_ins_bad_tier/register
Content-Type: application/json
{
  "address": "127.0.0.1:16381",
  "region": "us-west",
  "tier": "edge"
}
HTTP 400
[Asserts]
jsonpath "$.code" exists
jsonpath "$.message" exists

GET {{base_url}}/v1/instances/{{test_prefix}}_ins_root
HTTP 200
[Asserts]
jsonpath "$.id" == "{{test_prefix}}_ins_root"
jsonpath "$.status" exists

GET {{base_url}}/v1/instances/{{test_prefix}}_ins_missing
HTTP 404
[Asserts]
jsonpath "$.code" exists
jsonpath "$.message" exists

GET {{base_url}}/v1/instances
HTTP 200
[Asserts]
jsonpath "$.instances" exists
jsonpath "$.total" exists

GET {{base_url}}/v1/instances?status=active
HTTP 200
[Asserts]
jsonpath "$.instances" exists

GET {{base_url}}/v1/instances?region=us-west
HTTP 200
[Asserts]
jsonpath "$.instances" exists

POST {{base_url}}/v1/instances/{{test_prefix}}_ins_root/heartbeat
Content-Type: application/json
{
  "keyCount": 3,
  "totalSize": 4096,
  "hitRate": 0.5,
  "healthy": true
}
HTTP 200
[Asserts]
jsonpath "$.acknowledged" == true
jsonpath "$.nextHeartbeatMs" exists

POST {{base_url}}/v1/instances/{{test_prefix}}_ins_missing/heartbeat
Content-Type: application/json
{
  "healthy": true
}
HTTP 404
[Asserts]
jsonpath "$.code" exists
jsonpath "$.message" exists

DELETE {{base_url}}/v1/instances/{{test_prefix}}_ins_missing/deregister
HTTP 404
[Asserts]
jsonpath "$.code" exists
jsonpath "$.message" exists

DELETE {{base_url}}/v1/instances/{{test_prefix}}_ins_branch/deregister
HTTP 204

DELETE {{base_url}}/v1/instances/{{test_prefix}}_ins_root/deregister
HTTP 204
