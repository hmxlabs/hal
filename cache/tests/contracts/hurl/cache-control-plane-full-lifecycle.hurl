# Required variables:
# - base_url (for example http://localhost:8080)
# - run_id (unique suffix per run)

# 1) Baseline list (positive)
GET {{base_url}}/v1/instances
HTTP 200
[Asserts]
header "content-type" contains "application/json"
jsonpath "$.instances" count >= 0
jsonpath "$.total" >= 0

# 2) Register root, branch, and leaf (positive)
POST {{base_url}}/v1/instances/root-{{run_id}}/register
Content-Type: application/json
{
  "address": "127.0.0.1:6379",
  "region": "contract-test-region",
  "tier": "root",
  "maxSize": 1073741824,
  "metadata": {
    "suite": "hurl-contract"
  }
}
HTTP 201
[Asserts]
header "content-type" contains "application/json"
jsonpath "$.id" == "root-{{run_id}}"
jsonpath "$.registered" == true

POST {{base_url}}/v1/instances/branch-{{run_id}}/register
Content-Type: application/json
{
  "address": "127.0.0.1:6380",
  "region": "contract-test-region",
  "tier": "branch",
  "parentId": "root-{{run_id}}",
  "maxSize": 536870912,
  "metadata": {
    "suite": "hurl-contract"
  }
}
HTTP 201
[Asserts]
header "content-type" contains "application/json"
jsonpath "$.id" == "branch-{{run_id}}"
jsonpath "$.registered" == true

POST {{base_url}}/v1/instances/leaf-{{run_id}}/register
Content-Type: application/json
{
  "address": "127.0.0.1:6381",
  "region": "contract-test-region",
  "tier": "leaf",
  "parentId": "branch-{{run_id}}",
  "maxSize": 268435456,
  "metadata": {
    "suite": "hurl-contract"
  }
}
HTTP 201
[Asserts]
header "content-type" contains "application/json"
jsonpath "$.id" == "leaf-{{run_id}}"
jsonpath "$.registered" == true

# 3) Register negative cases
POST {{base_url}}/v1/instances/invalid-register-{{run_id}}/register
Content-Type: application/json
{
  "tier": "root"
}
HTTP 400
[Asserts]
header "content-type" contains "application/json"
jsonpath "$.code" exists
jsonpath "$.message" exists

POST {{base_url}}/v1/instances/root-{{run_id}}/register
Content-Type: application/json
{
  "address": "127.0.0.1:6379",
  "region": "contract-test-region",
  "tier": "root"
}
HTTP 409
[Asserts]
header "content-type" contains "application/json"
jsonpath "$.code" exists
jsonpath "$.message" exists

# 4) Instance reads and heartbeat (positive + negative)
GET {{base_url}}/v1/instances?status=active&region=contract-test-region
HTTP 200
[Asserts]
header "content-type" contains "application/json"
jsonpath "$.instances" count >= 0
jsonpath "$.total" >= 0

GET {{base_url}}/v1/instances/root-{{run_id}}
HTTP 200
[Asserts]
header "content-type" contains "application/json"
jsonpath "$.id" == "root-{{run_id}}"
jsonpath "$.tier" == "root"

GET {{base_url}}/v1/instances/does-not-exist-{{run_id}}
HTTP 404
[Asserts]
header "content-type" contains "application/json"
jsonpath "$.code" exists
jsonpath "$.message" exists

POST {{base_url}}/v1/instances/leaf-{{run_id}}/heartbeat
Content-Type: application/json
{
  "keyCount": 0,
  "totalSize": 0,
  "hitRate": 0.0,
  "cpuUsage": 0.1,
  "memoryUsage": 0.1,
  "healthy": true
}
HTTP 200
[Asserts]
header "content-type" contains "application/json"
jsonpath "$.acknowledged" == true
jsonpath "$.nextHeartbeatMs" > 0

POST {{base_url}}/v1/instances/does-not-exist-{{run_id}}/heartbeat
Content-Type: application/json
{
  "healthy": true
}
HTTP 404
[Asserts]
header "content-type" contains "application/json"
jsonpath "$.code" exists
jsonpath "$.message" exists

# 5) Topology and routing (positive + negative)
GET {{base_url}}/v1/topology
HTTP 200
[Asserts]
header "content-type" contains "application/json"
jsonpath "$.nodes" count >= 3
jsonpath "$.edges" count >= 2
jsonpath "$.rootId" exists

GET {{base_url}}/v1/topology/proximity
HTTP 200
[Asserts]
header "content-type" contains "application/json"
jsonpath "$.instances" count >= 3
jsonpath "$.matrix" count >= 3

GET {{base_url}}/v1/topology/proximity?instanceIds=root-{{run_id}},branch-{{run_id}},leaf-{{run_id}}
HTTP 200
[Asserts]
header "content-type" contains "application/json"
jsonpath "$.instances" count >= 3
jsonpath "$.matrix" count >= 3

GET {{base_url}}/v1/topology/routes/root-{{run_id}}/leaf-{{run_id}}
HTTP 200
[Asserts]
header "content-type" contains "application/json"
jsonpath "$.from" == "root-{{run_id}}"
jsonpath "$.to" == "leaf-{{run_id}}"
jsonpath "$.hops" count >= 2

GET {{base_url}}/v1/topology/routes/does-not-exist-{{run_id}}/root-{{run_id}}
HTTP 404
[Asserts]
header "content-type" contains "application/json"
jsonpath "$.code" exists
jsonpath "$.message" exists

# 6) Content inventory seed (positive + negative)
POST {{base_url}}/v1/content/instances/root-{{run_id}}/keys
Content-Type: application/json
{
  "mode": "full",
  "keys": [
    {
      "key": "inv-key-{{run_id}}",
      "size": 1024
    },
    {
      "key": "transfer-key-{{run_id}}",
      "size": 2048
    }
  ]
}
HTTP 200
[Asserts]
header "content-type" contains "application/json"
jsonpath "$.updated" >= 2
jsonpath "$.added" >= 2
jsonpath "$.removed" >= 0

POST {{base_url}}/v1/content/instances/root-{{run_id}}/keys
Content-Type: application/json
{
  "mode": "partial"
}
HTTP 400
[Asserts]
header "content-type" contains "application/json"
jsonpath "$.code" exists
jsonpath "$.message" exists

GET {{base_url}}/v1/content/instances/root-{{run_id}}/keys?limit=100
HTTP 200
[Asserts]
header "content-type" contains "application/json"
jsonpath "$.instanceId" == "root-{{run_id}}"
jsonpath "$.keys" count >= 2
jsonpath "$.total" >= 2

GET {{base_url}}/v1/content/instances/does-not-exist-{{run_id}}/keys
HTTP 404
[Asserts]
header "content-type" contains "application/json"
jsonpath "$.code" exists
jsonpath "$.message" exists

GET {{base_url}}/v1/content/locate/inv-key-{{run_id}}?sourceInstanceId=leaf-{{run_id}}
HTTP 200
[Asserts]
header "content-type" contains "application/json"
jsonpath "$.key" == "inv-key-{{run_id}}"
jsonpath "$.instances" count >= 1
jsonpath "$.instances[0].instanceId" == "root-{{run_id}}"

GET {{base_url}}/v1/content/nearest/inv-key-{{run_id}}?sourceInstanceId=leaf-{{run_id}}
HTTP 200
[Asserts]
header "content-type" contains "application/json"
jsonpath "$.key" == "inv-key-{{run_id}}"
jsonpath "$.instanceId" == "root-{{run_id}}"

GET {{base_url}}/v1/content/nearest/not-present-{{run_id}}?sourceInstanceId=leaf-{{run_id}}
HTTP 404
[Asserts]
header "content-type" contains "application/json"
jsonpath "$.code" exists
jsonpath "$.message" exists

# 7) Single-event state transitions (key_added, key_updated, key_evicted)
# key_added with sourceInstanceId asserts "obtained from another instance" state updates.
POST {{base_url}}/v1/events
Content-Type: application/json
{
  "instanceId": "leaf-{{run_id}}",
  "eventType": "key_added",
  "timestamp": "2026-01-01T00:00:01Z",
  "key": "transfer-key-{{run_id}}",
  "size": 2048,
  "sourceInstanceId": "root-{{run_id}}",
  "retrievalTimeMs": 12.5
}
HTTP 202
[Asserts]
header "content-type" contains "application/json"
jsonpath "$.accepted" == true
jsonpath "$.eventId" exists

# Poll leaf inventory until the add is visible.
GET {{base_url}}/v1/content/instances/leaf-{{run_id}}/keys?limit=100
[Options]
retry: 20
retry-interval: 250
HTTP 200
[Asserts]
header "content-type" contains "application/json"
jsonpath "$.instanceId" == "leaf-{{run_id}}"
jsonpath "$.total" == 1
jsonpath "$.keys[0].key" == "transfer-key-{{run_id}}"

GET {{base_url}}/v1/content/locate/transfer-key-{{run_id}}?sourceInstanceId=leaf-{{run_id}}
[Options]
retry: 20
retry-interval: 250
HTTP 200
[Asserts]
header "content-type" contains "application/json"
jsonpath "$.key" == "transfer-key-{{run_id}}"
jsonpath "$.instances" count >= 2

POST {{base_url}}/v1/events
Content-Type: application/json
{
  "instanceId": "root-{{run_id}}",
  "eventType": "key_updated",
  "timestamp": "2026-01-01T00:00:02Z",
  "key": "inv-key-{{run_id}}",
  "size": 4096
}
HTTP 202
[Asserts]
header "content-type" contains "application/json"
jsonpath "$.accepted" == true
jsonpath "$.eventId" exists

GET {{base_url}}/v1/content/locate/inv-key-{{run_id}}?sourceInstanceId=leaf-{{run_id}}
[Options]
retry: 20
retry-interval: 250
HTTP 200
[Asserts]
header "content-type" contains "application/json"
jsonpath "$.key" == "inv-key-{{run_id}}"
jsonpath "$.instances" count >= 1
jsonpath "$.instances[0].size" == 4096

POST {{base_url}}/v1/events
Content-Type: application/json
{
  "instanceId": "leaf-{{run_id}}",
  "eventType": "key_evicted",
  "timestamp": "2026-01-01T00:00:03Z",
  "key": "transfer-key-{{run_id}}"
}
HTTP 202
[Asserts]
header "content-type" contains "application/json"
jsonpath "$.accepted" == true
jsonpath "$.eventId" exists

# Poll until eviction is reflected for leaf inventory and locate map.
GET {{base_url}}/v1/content/instances/leaf-{{run_id}}/keys?limit=100
[Options]
retry: 20
retry-interval: 250
HTTP 200
[Asserts]
header "content-type" contains "application/json"
jsonpath "$.instanceId" == "leaf-{{run_id}}"
jsonpath "$.total" == 0

GET {{base_url}}/v1/content/locate/transfer-key-{{run_id}}?sourceInstanceId=leaf-{{run_id}}
[Options]
retry: 20
retry-interval: 250
HTTP 200
[Asserts]
header "content-type" contains "application/json"
jsonpath "$.key" == "transfer-key-{{run_id}}"
jsonpath "$.instances" count == 1
jsonpath "$.instances[0].instanceId" == "root-{{run_id}}"

# Negative single-event validation.
POST {{base_url}}/v1/events
Content-Type: application/json
{
  "instanceId": "leaf-{{run_id}}",
  "eventType": "key_added",
  "key": "invalid-event-key-{{run_id}}"
}
HTTP 400
[Asserts]
header "content-type" contains "application/json"
jsonpath "$.code" exists
jsonpath "$.message" exists

# 8) Batch events (positive + negative)
POST {{base_url}}/v1/events/batch
Content-Type: application/json
{
  "instanceId": "branch-{{run_id}}",
  "events": [
    {
      "eventType": "key_added",
      "timestamp": "2026-01-01T00:00:04Z",
      "key": "batch-key-a-{{run_id}}",
      "size": 111,
      "sourceInstanceId": "root-{{run_id}}",
      "retrievalTimeMs": 5.2
    },
    {
      "eventType": "key_added",
      "timestamp": "2026-01-01T00:00:05Z",
      "key": "batch-key-b-{{run_id}}",
      "size": 222,
      "sourceInstanceId": "root-{{run_id}}",
      "retrievalTimeMs": 6.4
    }
  ]
}
HTTP 202
[Asserts]
header "content-type" contains "application/json"
jsonpath "$.accepted" == true
jsonpath "$.count" == 2

GET {{base_url}}/v1/content/instances/branch-{{run_id}}/keys?limit=100
[Options]
retry: 20
retry-interval: 250
HTTP 200
[Asserts]
header "content-type" contains "application/json"
jsonpath "$.instanceId" == "branch-{{run_id}}"
jsonpath "$.total" == 2

POST {{base_url}}/v1/events/batch
Content-Type: application/json
{
  "instanceId": "branch-{{run_id}}",
  "events": [
    {
      "eventType": "key_evicted",
      "timestamp": "2026-01-01T00:00:06Z",
      "key": "batch-key-a-{{run_id}}"
    },
    {
      "eventType": "key_evicted",
      "timestamp": "2026-01-01T00:00:07Z",
      "key": "batch-key-b-{{run_id}}"
    }
  ]
}
HTTP 202
[Asserts]
header "content-type" contains "application/json"
jsonpath "$.accepted" == true
jsonpath "$.count" == 2

GET {{base_url}}/v1/content/instances/branch-{{run_id}}/keys?limit=100
[Options]
retry: 20
retry-interval: 250
HTTP 200
[Asserts]
header "content-type" contains "application/json"
jsonpath "$.instanceId" == "branch-{{run_id}}"
jsonpath "$.total" == 0

POST {{base_url}}/v1/events/batch
Content-Type: application/json
{
  "instanceId": "branch-{{run_id}}",
  "events": []
}
HTTP 400
[Asserts]
header "content-type" contains "application/json"
jsonpath "$.code" exists
jsonpath "$.message" exists

POST {{base_url}}/v1/events/batch
Content-Type: application/json
{
  "instanceId": "branch-{{run_id}}",
  "events": [
    {
      "eventType": "not-valid",
      "timestamp": "2026-01-01T00:00:08Z",
      "key": "bad-batch-key-{{run_id}}"
    }
  ]
}
HTTP 400
[Asserts]
header "content-type" contains "application/json"
jsonpath "$.code" exists
jsonpath "$.message" exists

# 9) Deregistration and content cleanup (positive + negative)
DELETE {{base_url}}/v1/instances/leaf-{{run_id}}/deregister
HTTP 204

DELETE {{base_url}}/v1/instances/leaf-{{run_id}}/deregister
HTTP 404
[Asserts]
header "content-type" contains "application/json"
jsonpath "$.code" exists
jsonpath "$.message" exists

GET {{base_url}}/v1/instances/leaf-{{run_id}}
HTTP 404
[Asserts]
header "content-type" contains "application/json"
jsonpath "$.code" exists
jsonpath "$.message" exists

DELETE {{base_url}}/v1/instances/branch-{{run_id}}/deregister
HTTP 204

DELETE {{base_url}}/v1/instances/root-{{run_id}}/deregister
HTTP 204

# Poll until root-owned data is removed from content map after deregistration.
GET {{base_url}}/v1/content/locate/inv-key-{{run_id}}
[Options]
retry: 20
retry-interval: 250
HTTP 200
[Asserts]
header "content-type" contains "application/json"
jsonpath "$.key" == "inv-key-{{run_id}}"
jsonpath "$.instances" count == 0

# Final smoke check.
GET {{base_url}}/v1/instances
HTTP 200
[Asserts]
header "content-type" contains "application/json"
jsonpath "$.instances" count >= 0
jsonpath "$.total" >= 0
