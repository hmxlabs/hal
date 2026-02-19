# RES-001 setup

POST {{base_url}}/v1/instances/{{test_prefix}}_res_root/register
Content-Type: application/json
{
  "address": "127.0.0.1:21379",
  "region": "us-west",
  "tier": "root",
  "maxSize": 1073741824
}
HTTP 201

POST {{base_url}}/v1/instances/{{test_prefix}}_res_branch/register
Content-Type: application/json
{
  "address": "127.0.0.1:21380",
  "region": "us-west",
  "tier": "branch",
  "parentId": "{{test_prefix}}_res_root",
  "maxSize": 536870912
}
HTTP 201

POST {{base_url}}/v1/instances/{{test_prefix}}_res_leaf/register
Content-Type: application/json
{
  "address": "127.0.0.1:21381",
  "region": "us-west",
  "tier": "leaf",
  "parentId": "{{test_prefix}}_res_branch",
  "maxSize": 268435456
}
HTTP 201

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
