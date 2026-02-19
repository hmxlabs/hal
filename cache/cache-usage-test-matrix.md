# HAL Cache Usage Scenario Mutation Matrix

This matrix covers end-to-end cache behavior scenarios that go beyond control-plane API contract testing.

The focus is validating:

- data propagation behavior in a root -> branch -> leaf hierarchy
- cache miss/hit behavior at each tier
- control-plane observability side effects (`/v1/content/*`, `/v1/events*`, `/v1/topology/*`)

## Mutation dimensions

| Dimension | Values |
|---|---|
| `request_tier` | `leaf`, `branch` |
| `key_origin_state` | `root_only`, `branch_only`, `leaf_only`, `multi_tier`, `absent` |
| `key_change_type` | `create`, `update`, `evict`, `delete`, `ttl_expire` |
| `candidate_sources` | `single`, `multiple_near`, `multiple_far` |
| `routing_input_quality` | `accurate`, `stale`, `missing` |
| `node_health` | `all_healthy`, `source_down`, `intermediate_down`, `requester_restarted` |
| `control_plane_state` | `available`, `unavailable`, `restarted` |
| `event_delivery` | `immediate`, `delayed`, `duplicate`, `out_of_order`, `batched` |
| `request_pattern` | `single_read`, `repeat_read`, `concurrent_reads`, `read_after_write` |
| `topology_shape` | `root->leaf`, `root->branch->leaf`, `root->branch->leaf + sibling` |

## Matrix

Legend:

- `Pri`: `P0` critical path, `P1` important, `P2` extended.
- `CP assertions` means validating via control-plane REST API.

| ID | Pri | Mutation vector | Scenario | Expected cache behavior | Expected CP assertions |
|---|---|---|---|---|---|
| USE-001 | P0 | `leaf`, `root_only`, `create`, `single`, `accurate`, `all_healthy`, `available`, `immediate`, `single_read`, `root->branch->leaf` | Write key at root, read once from leaf | Leaf miss -> fetch from hierarchy -> leaf stores -> returns value | `nearest` returns upstream source; `locate` includes leaf after read; `events` contains `key_added` with `sourceInstanceId` and `retrievalTimeMs` |
| USE-002 | P0 | `leaf`, `root_only`, `create`, `single`, `accurate`, `all_healthy`, `available`, `immediate`, `repeat_read`, `root->branch->leaf` | Same leaf reads same key twice | First read pull-through, second read local hit | First read updates `locate`; second read does not create duplicate add event for unchanged key |
| USE-003 | P0 | `leaf`, `branch_only`, `create`, `single`, `accurate`, `all_healthy`, `available`, `immediate`, `single_read`, `root->branch->leaf` | Key exists at branch only, leaf reads | Leaf fetches from branch (not root) | `nearest` for leaf resolves branch; `events` show `sourceInstanceId=branch` |
| USE-004 | P0 | `branch`, `root_only`, `create`, `single`, `accurate`, `all_healthy`, `available`, `immediate`, `single_read`, `root->branch->leaf` | Branch miss for root key | Branch fetches from root and stores locally | `locate` includes branch after fetch; add event emitted from branch |
| USE-005 | P0 | `leaf`, `absent`, `create`, `single`, `accurate`, `all_healthy`, `available`, `immediate`, `single_read`, `root->branch->leaf` | Leaf requests unknown key not present anywhere | Miss propagated, terminal not found | `nearest` returns `404`; no `key_added` event for leaf |
| USE-006 | P0 | `leaf`, `root_only`, `create`, `single`, `accurate`, `all_healthy`, `available`, `immediate`, `concurrent_reads`, `root->branch->leaf + sibling` | Two leaves concurrently request same root key | Both complete with value; each leaf caches local copy | `locate` includes both leaves; events include two `key_added` entries (one per leaf) |
| USE-007 | P1 | `leaf`, `multi_tier`, `create`, `multiple_near`, `accurate`, `all_healthy`, `available`, `immediate`, `single_read`, `root->branch->leaf + sibling` | Key exists in branch and sibling path | Requester uses nearest source by routing policy | `nearest` and route metrics align with chosen source |
| USE-008 | P1 | `leaf`, `multi_tier`, `create`, `multiple_far`, `accurate`, `all_healthy`, `available`, `immediate`, `single_read`, `root->branch->leaf + sibling` | Key exists in two sources with latency/cost tradeoff | Source selection respects policy weighting | `nearest` output and topology proximity explain choice |
| USE-009 | P1 | `leaf`, `root_only`, `create`, `single`, `stale`, `all_healthy`, `available`, `immediate`, `single_read`, `root->branch->leaf` | CP thinks branch has key but branch misses | Request eventually succeeds via root fallback | `nearest` may be wrong initially; subsequent events/inventory reconcile map |
| USE-010 | P1 | `leaf`, `root_only`, `create`, `single`, `missing`, `all_healthy`, `available`, `immediate`, `single_read`, `root->branch->leaf` | CP lacks mapping for existing root key | Cache walks parent path/fallback behavior and succeeds | Content map repaired after event/inventory update |
| USE-011 | P0 | `leaf`, `root_only`, `update`, `single`, `accurate`, `all_healthy`, `available`, `immediate`, `read_after_write`, `root->branch->leaf` | Root updates value after leaf cached old value | Leaf refreshes to new value on next miss/invalidation condition | CP inventory and/or events reflect latest size/version at leaf |
| USE-012 | P1 | `leaf`, `root_only`, `update`, `single`, `accurate`, `all_healthy`, `available`, `delayed`, `read_after_write`, `root->branch->leaf` | Update event delayed to CP | Data plane serves new value eventually; CP catches up later | Temporary map skew allowed; converges after delayed event |
| USE-013 | P1 | `leaf`, `root_only`, `update`, `single`, `accurate`, `all_healthy`, `available`, `duplicate`, `read_after_write`, `root->branch->leaf` | Duplicate update events delivered | Idempotent handling, no corruption | Duplicate events do not inflate key count or break map |
| USE-014 | P1 | `leaf`, `root_only`, `update`, `single`, `accurate`, `all_healthy`, `available`, `out_of_order`, `read_after_write`, `root->branch->leaf` | Out-of-order `key_updated` then older `key_added` | Final state remains latest | CP ends at latest timestamped state |
| USE-015 | P0 | `leaf`, `multi_tier`, `evict`, `single`, `accurate`, `all_healthy`, `available`, `immediate`, `repeat_read`, `root->branch->leaf` | Leaf evicts key; subsequent read requested | Leaf refetches and re-caches | `events` show `key_evicted` then `key_added`; `locate` reflects presence after refetch |
| USE-016 | P1 | `leaf`, `multi_tier`, `ttl_expire`, `single`, `accurate`, `all_healthy`, `available`, `immediate`, `repeat_read`, `root->branch->leaf` | Leaf key expires by TTL | Refill from nearest source on next read | Eviction/expiry reflected in CP by event or inventory reconcile |
| USE-017 | P1 | `leaf`, `root_only`, `delete`, `single`, `accurate`, `all_healthy`, `available`, `immediate`, `read_after_write`, `root->branch->leaf` | Root deletes key after downstream cached copies | Downstream behavior follows configured invalidation policy | CP map converges to policy outcome (removed or stale-until-expire) |
| USE-018 | P0 | `leaf`, `root_only`, `create`, `single`, `accurate`, `source_down`, `available`, `immediate`, `single_read`, `root->branch->leaf` | Nearest source unavailable during fetch | Failover to alternate source or terminal failure if none | Routing/availability reflected; error or fallback source visible in events |
| USE-019 | P0 | `leaf`, `root_only`, `create`, `single`, `accurate`, `intermediate_down`, `available`, `immediate`, `single_read`, `root->branch->leaf` | Branch down while root has key | Behavior follows failover policy (bypass or fail) | CP instance status influences routing choice |
| USE-020 | P0 | `leaf`, `root_only`, `create`, `single`, `accurate`, `all_healthy`, `unavailable`, `immediate`, `single_read`, `root->branch->leaf` | Control plane unavailable at miss time | Cache falls back to parent traversal and still serves when possible | Buffered events replay after CP restore; map converges |
| USE-021 | P0 | `leaf`, `multi_tier`, `create`, `single`, `accurate`, `requester_restarted`, `available`, `immediate`, `repeat_read`, `root->branch->leaf` | Leaf restart and read key again | Local cache state consistent with persistence policy | Re-registration + heartbeat + inventory restore map correctness |
| USE-022 | P0 | `leaf`, `root_only`, `create`, `single`, `accurate`, `all_healthy`, `restarted`, `immediate`, `single_read`, `root->branch->leaf` | CP restart between writes and reads | Data serving continues; control plane recovers mapping | `instances`, `topology`, `locate` converge after recovery |
| USE-023 | P1 | `leaf`, `root_only`, `create`, `single`, `stale`, `all_healthy`, `restarted`, `delayed`, `single_read`, `root->branch->leaf` | CP restart + stale map + delayed events | Data eventually served; control-plane eventual consistency | Reconciliation removes stale locations and adds true holders |
| USE-024 | P1 | `leaf`, `root_only`, `create`, `single`, `accurate`, `all_healthy`, `available`, `batched`, `concurrent_reads`, `root->branch->leaf + sibling` | High-rate adds emitted in batches | Data available at readers; batching does not lose updates | Batch event counts match observed key placements |
| USE-025 | P1 | `leaf`, `root_only`, `create`, `single`, `accurate`, `all_healthy`, `available`, `duplicate`, `concurrent_reads`, `root->branch->leaf + sibling` | Retry causes duplicate batch submission | Idempotent event processing | No duplicate holder entries or incorrect key counts |
| USE-026 | P1 | `leaf`, `root_only`, `create`, `single`, `accurate`, `all_healthy`, `available`, `out_of_order`, `concurrent_reads`, `root->branch->leaf + sibling` | Batches arrive out of order | Final placement map converges correctly | Latest event ordering strategy verified |
| USE-027 | P1 | `leaf`, `multi_tier`, `update`, `multiple_near`, `accurate`, `all_healthy`, `available`, `immediate`, `concurrent_reads`, `root->branch->leaf + sibling` | Concurrent reads during upstream update | Readers observe policy-consistent value transition | CP shows no impossible state transitions |
| USE-028 | P1 | `branch`, `root_only`, `create`, `single`, `accurate`, `all_healthy`, `available`, `immediate`, `concurrent_reads`, `root->branch->leaf` | Many leaves under one branch request same cold key | Branch acts as fan-in cache, reducing root pulls | Root-to-branch transfer count bounded; branch->leaf events scale linearly with leaves |
| USE-029 | P1 | `leaf`, `multi_tier`, `evict`, `multiple_near`, `accurate`, `all_healthy`, `available`, `immediate`, `concurrent_reads`, `root->branch->leaf + sibling` | Eviction storm while readers active | No corrupted values; eventual refill works | CP receives coherent add/evict sequence per instance |
| USE-030 | P2 | `leaf`, `root_only`, `create`, `multiple_far`, `accurate`, `all_healthy`, `available`, `immediate`, `single_read`, `root->branch->leaf + sibling` | Cross-region source candidate available | Routing prefers lower cost/latency policy as configured | `nearest` and `route` metrics validate decision |
| USE-031 | P2 | `leaf`, `root_only`, `create`, `multiple_far`, `stale`, `all_healthy`, `available`, `immediate`, `single_read`, `root->branch->leaf + sibling` | Cross-region stale proximity matrix | Wrong source may be selected initially; self-healing expected | Metrics and subsequent map updates correct the path |
| USE-032 | P1 | `leaf`, `leaf_only`, `create`, `single`, `accurate`, `all_healthy`, `available`, `immediate`, `single_read`, `root->branch->leaf + sibling` | Key exists in sibling leaf only | Request reaches sibling path only if policy allows peer sourcing | `nearest` and `route` behavior matches topology rules |
| USE-033 | P1 | `leaf`, `leaf_only`, `create`, `single`, `accurate`, `all_healthy`, `available`, `immediate`, `single_read`, `root->branch->leaf` | Key exists only in requester leaf | Pure local hit | No upstream fetch; no new add event for unchanged key |
| USE-034 | P2 | `branch`, `multi_tier`, `evict`, `single`, `accurate`, `all_healthy`, `available`, `immediate`, `repeat_read`, `root->branch->leaf` | Branch evicts while leaf still holds key | Leaf can serve local hits until its own miss/expiry | CP `locate` removes branch holder but retains leaf holder |
| USE-035 | P2 | `leaf`, `root_only`, `create`, `single`, `accurate`, `all_healthy`, `available`, `immediate`, `single_read`, `root->leaf` | Two-tier topology (no branch) | Direct root->leaf pull-through works | CP topology and route reflect two-hop graph |
| USE-036 | P2 | `leaf`, `root_only`, `create`, `single`, `accurate`, `all_healthy`, `available`, `immediate`, `single_read`, `root->branch->leaf + sibling` | Deeper topology variant with extra branch level | Pull-through follows nearest valid ancestor path | CP route hop list matches configured graph |
| USE-037 | P1 | `leaf`, `root_only`, `create`, `single`, `accurate`, `all_healthy`, `available`, `immediate`, `read_after_write`, `root->branch->leaf` | Root write followed immediately by leaf read | No stale not-found race beyond allowed window | CP locate transitions from root-only to root+leaf |
| USE-038 | P1 | `leaf`, `root_only`, `create`, `single`, `accurate`, `all_healthy`, `available`, `delayed`, `read_after_write`, `root->branch->leaf` | Write visible in data plane before event ingestion | Read succeeds even when map lags | CP catches up after delayed event |
| USE-039 | P2 | `leaf`, `absent`, `delete`, `single`, `accurate`, `all_healthy`, `available`, `immediate`, `single_read`, `root->branch->leaf` | Delete non-existent key and read | Stable not-found behavior | CP map unchanged |
| USE-040 | P2 | `leaf`, `multi_tier`, `update`, `single`, `accurate`, `all_healthy`, `available`, `batched`, `concurrent_reads`, `root->branch->leaf + sibling` | Mixed add/update/evict in same batch window | Final value/version consistent at leaves | Batch accounting and final `locate` state agree |

## Coverage notes

- This matrix defines the complete mutation space by dimension and a canonical scenario set that covers every mutation value in realistic combinations.
- For strict combinatorial coverage, generate pairwise or 3-way combinations from the mutation dimensions and map each generated case to this ID scheme (`USE-*`).
- If data consistency policy for `delete`/`update` invalidation is not yet fixed, keep both policy branches as separate expected outcomes and mark one as required once policy is finalized.
