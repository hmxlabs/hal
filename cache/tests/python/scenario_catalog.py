from dataclasses import dataclass
from typing import Optional


@dataclass(frozen=True)
class Scenario:
    id: str
    priority: str
    title: str
    executor: Optional[str]
    requires_sibling_leaf: bool = False


SCENARIOS = [
    Scenario("USE-001", "P0", "Root write then leaf read propagates and updates control plane", "use_001"),
    Scenario("USE-002", "P0", "Second leaf read is local hit after initial pull-through", "use_002"),
    Scenario("USE-003", "P0", "Leaf sources from branch when key is branch-only", "use_003"),
    Scenario("USE-004", "P0", "Branch sources from root on miss", "use_004"),
    Scenario("USE-005", "P0", "Unknown key returns miss/not-found with no propagation", "use_005"),
    Scenario("USE-006", "P0", "Concurrent reads from two leaves propagate to both", "use_006", True),
    Scenario("USE-007", "P1", "Nearest source selection with multiple near candidates", None, True),
    Scenario("USE-008", "P1", "Routing decision with latency/cost tradeoff", None, True),
    Scenario("USE-009", "P1", "Stale control-plane mapping self-heals via fallback", None),
    Scenario("USE-010", "P1", "Missing control-plane mapping repaired by reconciliation", None),
    Scenario("USE-011", "P0", "Root update after leaf cache population refreshes downstream", None),
    Scenario("USE-012", "P1", "Delayed event delivery converges eventually", None),
    Scenario("USE-013", "P1", "Duplicate update events are idempotent", None),
    Scenario("USE-014", "P1", "Out-of-order updates converge to latest state", None),
    Scenario("USE-015", "P0", "Leaf eviction then refetch emits coherent state", None),
    Scenario("USE-016", "P1", "TTL expiry at leaf triggers refill on demand", None),
    Scenario("USE-017", "P1", "Root delete behavior matches invalidation policy", None),
    Scenario("USE-018", "P0", "Source down failover or terminal miss", None),
    Scenario("USE-019", "P0", "Intermediate down behavior follows failover policy", None),
    Scenario("USE-020", "P0", "Control-plane unavailable fallback and replay", None),
    Scenario("USE-021", "P0", "Requester restart preserves/recovers local cache policy", None),
    Scenario("USE-022", "P0", "Control-plane restart converges state", None),
    Scenario("USE-023", "P1", "Restart with stale map and delayed events converges", None),
    Scenario("USE-024", "P1", "Batched high-rate adds do not lose updates", None, True),
    Scenario("USE-025", "P1", "Duplicate batched submissions remain idempotent", None, True),
    Scenario("USE-026", "P1", "Out-of-order batches converge", None, True),
    Scenario("USE-027", "P1", "Concurrent reads during update are policy-consistent", None, True),
    Scenario("USE-028", "P1", "Branch fan-in reduces root pulls", None),
    Scenario("USE-029", "P1", "Eviction storm remains coherent", None, True),
    Scenario("USE-030", "P2", "Cross-region route selection matches policy", None, True),
    Scenario("USE-031", "P2", "Stale proximity matrix self-corrects", None, True),
    Scenario("USE-032", "P1", "Sibling leaf sourcing matches topology policy", None, True),
    Scenario("USE-033", "P1", "Requester-local leaf hit has no upstream fetch", None),
    Scenario("USE-034", "P2", "Branch eviction retains leaf holder visibility", None),
    Scenario("USE-035", "P2", "Two-tier root->leaf topology pull-through", None),
    Scenario("USE-036", "P2", "Deeper topology route hop correctness", None, True),
    Scenario("USE-037", "P1", "Immediate read-after-write avoids stale not-found", None),
    Scenario("USE-038", "P1", "Data plane ahead of event ingestion still serves", None),
    Scenario("USE-039", "P2", "Delete non-existent key leaves stable miss state", None),
    Scenario("USE-040", "P2", "Mixed add/update/evict batch window converges", None, True),
]


SCENARIO_BY_ID = {s.id: s for s in SCENARIOS}
