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
    Scenario("USE-007", "P1", "Nearest source selection with multiple near candidates", "use_007", True),
    Scenario("USE-008", "P1", "Routing decision with latency/cost tradeoff", "use_008", True),
    Scenario("USE-009", "P1", "Stale control-plane mapping self-heals via fallback", "use_009"),
    Scenario("USE-010", "P1", "Missing control-plane mapping repaired by reconciliation", "use_010"),
    Scenario("USE-011", "P0", "Root update after leaf cache population refreshes downstream", "use_011"),
    Scenario("USE-012", "P1", "Delayed event delivery converges eventually", "use_012"),
    Scenario("USE-013", "P1", "Duplicate update events are idempotent", "use_013"),
    Scenario("USE-014", "P1", "Out-of-order updates converge to latest state", "use_014"),
    Scenario("USE-015", "P0", "Leaf eviction then refetch emits coherent state", "use_015"),
    Scenario("USE-016", "P1", "TTL expiry at leaf triggers refill on demand", "use_016"),
    Scenario("USE-017", "P1", "Root delete behavior matches invalidation policy", "use_017"),
    Scenario("USE-018", "P0", "Source down failover or terminal miss", "use_018"),
    Scenario("USE-019", "P0", "Intermediate down behavior follows failover policy", "use_019"),
    Scenario("USE-020", "P0", "Control-plane unavailable fallback and replay", "use_020"),
    Scenario("USE-021", "P0", "Requester restart preserves/recovers local cache policy", "use_021"),
    Scenario("USE-022", "P0", "Control-plane restart converges state", "use_022"),
    Scenario("USE-023", "P1", "Restart with stale map and delayed events converges", "use_023"),
    Scenario("USE-024", "P1", "Batched high-rate adds do not lose updates", "use_024", True),
    Scenario("USE-025", "P1", "Duplicate batched submissions remain idempotent", "use_025", True),
    Scenario("USE-026", "P1", "Out-of-order batches converge", "use_026", True),
    Scenario("USE-027", "P1", "Concurrent reads during update are policy-consistent", "use_027", True),
    Scenario("USE-028", "P1", "Branch fan-in reduces root pulls", "use_028"),
    Scenario("USE-029", "P1", "Eviction storm remains coherent", "use_029", True),
    Scenario("USE-030", "P2", "Cross-region route selection matches policy", "use_030", True),
    Scenario("USE-031", "P2", "Stale proximity matrix self-corrects", "use_031", True),
    Scenario("USE-032", "P1", "Sibling leaf sourcing matches topology policy", "use_032", True),
    Scenario("USE-033", "P1", "Requester-local leaf hit has no upstream fetch", "use_033"),
    Scenario("USE-034", "P2", "Branch eviction retains leaf holder visibility", "use_034"),
    Scenario("USE-035", "P2", "Two-tier root->leaf topology pull-through", "use_035"),
    Scenario("USE-036", "P2", "Deeper topology route hop correctness", "use_036", True),
    Scenario("USE-037", "P1", "Immediate read-after-write avoids stale not-found", "use_037"),
    Scenario("USE-038", "P1", "Data plane ahead of event ingestion still serves", "use_038"),
    Scenario("USE-039", "P2", "Delete non-existent key leaves stable miss state", "use_039"),
    Scenario("USE-040", "P2", "Mixed add/update/evict batch window converges", "use_040", True),
]


SCENARIO_BY_ID = {s.id: s for s in SCENARIOS}
