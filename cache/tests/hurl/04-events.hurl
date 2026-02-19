# EVT-001, EVT-002, EVT-003, EVT-004, EVT-006

POST {{base_url}}/v1/instances/{{test_prefix}}_evt_root/register
Content-Type: application/json
{
  "address": "127.0.0.1:19379",
  "region": "us-west",
  "tier": "root",
  "maxSize": 1073741824
}
HTTP 201

POST {{base_url}}/v1/events
Content-Type: application/json
{
  "instanceId": "{{test_prefix}}_evt_root",
  "eventType": "key_added",
  "timestamp": "2026-01-01T00:00:00Z",
  "key": "dataset_imagenet_v1",
  "size": 100,
  "sourceInstanceId": "{{test_prefix}}_evt_root",
  "retrievalTimeMs": 1.2
}
HTTP 202
[Asserts]
jsonpath "$.accepted" == true
jsonpath "$.eventId" exists

POST {{base_url}}/v1/events
Content-Type: application/json
{
  "instanceId": "{{test_prefix}}_evt_root",
  "eventType": "key_removed",
  "timestamp": "2026-01-01T00:00:00Z",
  "key": "dataset_imagenet_v1"
}
HTTP 400
[Asserts]
jsonpath "$.code" exists
jsonpath "$.message" exists

POST {{base_url}}/v1/events/batch
Content-Type: application/json
{
  "instanceId": "{{test_prefix}}_evt_root",
  "events": [
    {
      "eventType": "key_added",
      "timestamp": "2026-01-01T00:00:00Z",
      "key": "k1",
      "size": 1
    },
    {
      "eventType": "key_updated",
      "timestamp": "2026-01-01T00:00:00Z",
      "key": "k2",
      "size": 2
    }
  ]
}
HTTP 202
[Asserts]
jsonpath "$.accepted" == true
jsonpath "$.count" == 2

POST {{base_url}}/v1/events/batch
Content-Type: application/json
{
  "instanceId": "{{test_prefix}}_evt_root",
  "events": []
}
HTTP 400
[Asserts]
jsonpath "$.code" exists
jsonpath "$.message" exists

POST {{base_url}}/v1/events/batch
Content-Type: application/json
{
  "instanceId": "{{test_prefix}}_evt_root",
  "events": [
    {
      "eventType": "key_added",
      "timestamp": "2026-01-01T00:00:00Z",
      "key": "mk1",
      "size": 10
    },
    {
      "eventType": "key_updated",
      "timestamp": "2026-01-01T00:00:00Z",
      "key": "mk2",
      "size": 20
    },
    {
      "eventType": "key_evicted",
      "timestamp": "2026-01-01T00:00:00Z",
      "key": "mk3"
    }
  ]
}
HTTP 202
[Asserts]
jsonpath "$.accepted" == true
jsonpath "$.count" == 3

DELETE {{base_url}}/v1/instances/{{test_prefix}}_evt_root/deregister
HTTP 204
