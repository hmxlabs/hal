# CNT-001, CNT-002, CNT-003, CNT-004, CNT-005, CNT-006, CNT-007, CNT-008, CNT-009

POST {{base_url}}/v1/instances/{{test_prefix}}_cnt_root/register
Content-Type: application/json
{
  "address": "127.0.0.1:17379",
  "region": "us-west",
  "tier": "root",
  "maxSize": 1073741824
}
HTTP 201

POST {{base_url}}/v1/instances/{{test_prefix}}_cnt_branch/register
Content-Type: application/json
{
  "address": "127.0.0.1:17380",
  "region": "us-west",
  "tier": "branch",
  "parentId": "{{test_prefix}}_cnt_root",
  "maxSize": 536870912
}
HTTP 201

POST {{base_url}}/v1/instances/{{test_prefix}}_cnt_leaf/register
Content-Type: application/json
{
  "address": "127.0.0.1:17381",
  "region": "us-west",
  "tier": "leaf",
  "parentId": "{{test_prefix}}_cnt_branch",
  "maxSize": 268435456
}
HTTP 201

POST {{base_url}}/v1/content/instances/{{test_prefix}}_cnt_root/keys
Content-Type: application/json
{
  "mode": "partial",
  "keys": [
    {
      "key": "dataset_imagenet_v1",
      "size": 1000,
      "lastAccessed": "2026-01-01T00:00:00Z"
    },
    {
      "key": "dataset_llama2_tokenizer",
      "size": 2000,
      "lastAccessed": "2026-01-01T00:00:00Z"
    }
  ]
}
HTTP 200
[Asserts]
jsonpath "$.updated" >= 2
jsonpath "$.added" exists

POST {{base_url}}/v1/content/instances/{{test_prefix}}_cnt_root/keys
Content-Type: application/json
{
  "mode": "full",
  "keys": [
    {
      "key": "checkpoint_resnet50_epoch10",
      "size": 3000,
      "lastAccessed": "2026-01-01T00:00:00Z"
    }
  ]
}
HTTP 200
[Asserts]
jsonpath "$.updated" >= 1
jsonpath "$.removed" exists

POST {{base_url}}/v1/content/instances/{{test_prefix}}_cnt_branch/keys
Content-Type: application/json
{
  "mode": "partial",
  "keys": [
    {
      "key": "checkpoint_resnet50_epoch10",
      "size": 3000,
      "lastAccessed": "2026-01-01T00:00:00Z"
    }
  ]
}
HTTP 200

GET {{base_url}}/v1/content/instances/{{test_prefix}}_cnt_root/keys
HTTP 200
[Asserts]
jsonpath "$.instanceId" == "{{test_prefix}}_cnt_root"
jsonpath "$.keys" exists
jsonpath "$.total" exists

GET {{base_url}}/v1/content/instances/{{test_prefix}}_cnt_root/keys?limit=1
HTTP 200
[Asserts]
jsonpath "$.instanceId" == "{{test_prefix}}_cnt_root"
jsonpath "$.keys" exists

GET {{base_url}}/v1/content/locate/checkpoint_resnet50_epoch10
HTTP 200
[Asserts]
jsonpath "$.key" == "checkpoint_resnet50_epoch10"
jsonpath "$.instances" exists

GET {{base_url}}/v1/content/locate/checkpoint_resnet50_epoch10?sourceInstanceId={{test_prefix}}_cnt_leaf
HTTP 200
[Asserts]
jsonpath "$.instances" exists

GET {{base_url}}/v1/content/nearest/checkpoint_resnet50_epoch10?sourceInstanceId={{test_prefix}}_cnt_leaf
HTTP 200
[Asserts]
jsonpath "$.key" == "checkpoint_resnet50_epoch10"
jsonpath "$.instanceId" exists

GET {{base_url}}/v1/content/nearest/checkpoint_resnet50_epoch10
HTTP 400
[Asserts]
jsonpath "$.code" exists
jsonpath "$.message" exists

GET {{base_url}}/v1/content/nearest/key_that_does_not_exist?sourceInstanceId={{test_prefix}}_cnt_leaf
HTTP 404
[Asserts]
jsonpath "$.code" exists
jsonpath "$.message" exists

DELETE {{base_url}}/v1/instances/{{test_prefix}}_cnt_leaf/deregister
HTTP 204

DELETE {{base_url}}/v1/instances/{{test_prefix}}_cnt_branch/deregister
HTTP 204

DELETE {{base_url}}/v1/instances/{{test_prefix}}_cnt_root/deregister
HTTP 204
