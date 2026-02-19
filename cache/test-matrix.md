# HAL Cache Control Plane Test Matrix

This matrix is derived from `cache/openapi.yaml` and is the executable checklist for `cache/tests.md`.
It covers control-plane API behavior only; end-to-end cache data propagation scenarios are tracked in `cache/cache-usage-test-matrix.md`.

## Legend

- `Layer`: contract, endpoint, integration, resilience, performance
- `Pri`: P0 (highest) to P2 (lowest)

| ID | Pri | Layer | Target | Scenario | Input / Setup | Expected |
|---|---|---|---|---|---|---|
| CT-001 | P0 | contract | `openapi.yaml` | OpenAPI document is valid | Parse and lint spec | No validation errors |
| CT-002 | P0 | contract | `openapi.yaml` | All operations have unique `operationId` | Static check | No duplicates |
| CT-003 | P0 | contract | `components.schemas.Error` | Error schema requires `code` and `message` | Schema validation | Contract enforced |
| INS-001 | P0 | endpoint | `POST /v1/instances/{id}/register` | Register root instance | `id=root-us-east`, body with `address`, `tier=root` | `201`, body has `id`, `registered=true` |
| INS-002 | P0 | endpoint | `POST /v1/instances/{id}/register` | Register branch instance | `id=branch-us-west`, `tier=branch`, valid `parentId` | `201` |
| INS-003 | P0 | endpoint | `POST /v1/instances/{id}/register` | Duplicate registration | Register same `id` twice | Second request `409` with `Error` schema |
| INS-004 | P0 | endpoint | `POST /v1/instances/{id}/register` | Invalid `tier` enum | `tier=edge` | `400` with `Error` schema |
| INS-005 | P0 | endpoint | `GET /v1/instances/{id}` | Get existing instance | Existing `id` | `200`, matches `CacheInstanceDetail` |
| INS-006 | P0 | endpoint | `GET /v1/instances/{id}` | Get missing instance | Unknown `id` | `404` with `Error` schema |
| INS-007 | P1 | endpoint | `GET /v1/instances` | List all instances | No filters | `200`, `instances[]`, `total` |
| INS-008 | P1 | endpoint | `GET /v1/instances` | Filter by status | `?status=active` | `200`, all items `status=active` |
| INS-009 | P1 | endpoint | `GET /v1/instances` | Filter by region | `?region=us-west` | `200`, filtered set |
| INS-010 | P0 | endpoint | `POST /v1/instances/{id}/heartbeat` | Heartbeat acknowledged | Existing `id`, heartbeat payload | `200`, `acknowledged=true`, `nextHeartbeatMs` present |
| INS-011 | P0 | endpoint | `POST /v1/instances/{id}/heartbeat` | Heartbeat for missing instance | Unknown `id` | `404` |
| INS-012 | P0 | endpoint | `DELETE /v1/instances/{id}/deregister` | Deregister instance | Existing `id` | `204` |
| INS-013 | P0 | endpoint | `DELETE /v1/instances/{id}/deregister` | Deregister missing instance | Unknown `id` | `404` |
| VAL-001 | P0 | endpoint | `InstanceId` parameter | Reject invalid ID format | `id=bad$id` | `400` or route-level rejection mapped to `Error` |
| CNT-001 | P0 | endpoint | `POST /v1/content/instances/{id}/keys` | Partial key update upserts keys | `mode=partial`, 2 keys | `200`, `updated>=2`, `added>=0` |
| CNT-002 | P0 | endpoint | `POST /v1/content/instances/{id}/keys` | Full key update replaces inventory | `mode=full`, submit new set | `200`, `removed` reflects replaced keys |
| CNT-003 | P1 | endpoint | `GET /v1/content/instances/{id}/keys` | List keys default pagination | Existing `id` | `200`, `keys[]`, optional `cursor`, `total` |
| CNT-004 | P1 | endpoint | `GET /v1/content/instances/{id}/keys` | List keys with cursor and limit | `?cursor=...&limit=50` | `200`, page size <= 50 |
| CNT-005 | P0 | endpoint | `GET /v1/content/locate/{key}` | Locate key holders | Existing key | `200`, `instances[]` non-empty |
| CNT-006 | P0 | endpoint | `GET /v1/content/locate/{key}` | Locate with source ordering | `?sourceInstanceId=leaf-us-west-1` | `200`, list ordered by proximity |
| CNT-007 | P0 | endpoint | `GET /v1/content/nearest/{key}` | Nearest source success | Existing key + source instance | `200`, `instanceId` present, optional proximity fields valid |
| CNT-008 | P0 | endpoint | `GET /v1/content/nearest/{key}` | Missing required query | No `sourceInstanceId` | `400` |
| CNT-009 | P0 | endpoint | `GET /v1/content/nearest/{key}` | Key not found | Unknown key + valid source | `404` with `Error` schema |
| TOP-001 | P0 | endpoint | `GET /v1/topology` | Fetch topology graph | Registered hierarchy exists | `200`, has `nodes[]`, `edges[]` |
| TOP-002 | P1 | endpoint | `GET /v1/topology/proximity` | Full proximity matrix | No filter | `200`, matrix dimensions match instances |
| TOP-003 | P1 | endpoint | `GET /v1/topology/proximity` | Filtered proximity matrix | `?instanceIds=root-us-east,leaf-us-west-1` | `200`, matrix only for requested instances |
| TOP-004 | P0 | endpoint | `GET /v1/topology/routes/{from}/{to}` | Route success | Valid `from` and `to` | `200`, `hops[]` non-empty |
| TOP-005 | P0 | endpoint | `GET /v1/topology/routes/{from}/{to}` | Route for unknown instance | Unknown `from` or `to` | `404` with `Error` schema |
| EVT-001 | P0 | endpoint | `POST /v1/events` | Submit single `key_added` event | Valid `CacheEvent` payload | `202`, `accepted=true`, `eventId` present |
| EVT-002 | P0 | endpoint | `POST /v1/events` | Reject invalid `eventType` | `eventType=key_removed` | `400` |
| EVT-003 | P0 | endpoint | `POST /v1/events/batch` | Submit valid batch | 2 valid `events[]` entries | `202`, `accepted=true`, `count=2` |
| EVT-004 | P0 | endpoint | `POST /v1/events/batch` | Reject empty batch | `events=[]` | `400` |
| EVT-005 | P0 | endpoint | `POST /v1/events/batch` | Reject oversize batch | `events` length `10001` | `400` |
| EVT-006 | P1 | endpoint | `POST /v1/events/batch` | Mixed event types accepted | `key_added`, `key_updated`, `key_evicted` | `202`, `count` equals submitted events |
| INT-001 | P0 | integration | Lifecycle workflow | Register -> heartbeat -> key update -> nearest lookup | Canonical fixtures | All requests succeed and state is queryable |
| INT-002 | P0 | integration | Event updates content visibility | Add key via inventory/event then locate | Existing hierarchy | `locate`/`nearest` reflect latest source |
| INT-003 | P0 | integration | Deregister removes availability | Deregister instance holding key | Instance removed | Key no longer returned for that instance |
| INT-004 | P1 | integration | Topology consistency after registration | Add branch + leaf nodes | Query topology/routes | New nodes/edges appear and route resolves |
| RES-001 | P0 | resilience | Control plane restart | Persist state, restart service | Registry/topology restored and queryable |
| RES-002 | P1 | resilience | Reconciliation after restart | Re-submit inventories/events | Content map converges without schema errors |
| PERF-001 | P1 | performance | `POST /v1/events/batch` throughput | Sustained batch ingestion load | Meets agreed throughput SLO |
| PERF-002 | P1 | performance | `GET /v1/content/nearest/{key}` latency | Large key set, concurrent lookups | Meets p95 latency SLO |
| PERF-003 | P2 | performance | `GET /v1/topology/routes/{from}/{to}` latency | Large topology graph | Meets p95 latency SLO |

## Notes

- Where status code behavior can differ by framework for malformed paths, normalize responses to OpenAPI `Error` schema in adapters/middleware.
- If business rules are implemented beyond schema (for example `parentId` enforcement for non-root tiers), add explicit validation tests and pin expected status codes.
