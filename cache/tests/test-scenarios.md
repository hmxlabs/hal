# HAL Cache Test Scenarios

This document tracks end-to-end scenarios executed by the Python test framework in `cache/tests/hal_cache_test_framework.py`.

## Use Case 1: Single Root Cache Instance

Validate behavior when only one cache node exists (the root), and confirm the control plane reflects write/read actions correctly.

### Topology Diagram

```text
┌───────────────────────┐
│ Test Framework Client │
└───────────┬───────────┘
            │ read/write (Redis)
            v
     ┌───────────────┐
     │ Root Cache    │
     │ (root1)       │
     └───────────────┘
```

### Scenario File

- `cache/tests/single_root_scenarios.json`

### Included Scenarios

1. `root_only_write_and_locate`
2. `root_only_read_after_write`
3. `root_only_overwrite_latest_value`
4. `root_only_delete_and_recreate`

### Validation Goals

- Writes to `root1` succeed and are visible through control plane key-location APIs.
- Reads from `root1` return expected values and preserve correct holder mapping.
- Overwrites return the latest value on subsequent reads.
- Delete + recreate cycles converge to the recreated value and correct control plane holder state.

## Use Case 2: Root Cache + Single Leaf Cache

Validate pull-through behavior when one leaf cache reads data that exists at the root cache.

### Topology Diagram

```text
┌───────────────────────┐
│ Test Framework Client │
└───────────┬───────────┘
            │ read/write (Redis)
            v
     ┌───────────────┐
     │ Leaf Cache    │
     │ (leaf1)       │
     └──────┬────────┘
            │ cache miss fetch
            v
     ┌───────────────┐
     │ Root Cache    │
     │ (root1)       │
     └───────────────┘
```

### Scenario Intent

1. Write a key to `root1`.
2. Confirm control plane lists `root1` as a holder.
3. Read the key from `leaf1` and verify expected value.
4. Confirm control plane now lists both `root1` and `leaf1` as holders.

### Scenario File

- `cache/tests/root_leaf_scenarios.json`

### Included Scenarios

1. `root_leaf_pullthrough_basic` (positive)
2. `root_leaf_repeat_read_stays_consistent` (positive)
3. `negative_leaf_read_missing_key_should_fail` (negative, expected failure)
4. `negative_leaf_read_wrong_expected_value_should_fail` (negative, expected failure)
5. `negative_require_state_change_twice_should_fail` (negative, expected failure)

### Negative Scenario Note

- Negative scenarios are intentionally expected to fail specific operations.
- They are asserted either with scenario-level `expected_failure: true` or step-level `write_expect_failure`, so full files can include both positive and negative scenarios.

### Validation Goals

- Leaf miss triggers upstream retrieval from root.
- Leaf caches the fetched value locally after the read.
- Control plane location state converges from `root-only` to `root+leaf`.

## Use Case 3: Root Cache + Branch Cache + Leaf Cache

Validate multi-hop pull-through behavior where a leaf requests data and the request traverses a branch cache to the root cache.

### Topology Diagram

```text
┌───────────────────────┐
│ Test Framework Client │
└───────────┬───────────┘
            │ read/write (Redis)
            v
     ┌───────────────┐
     │ Leaf Cache    │
     │ (leaf1)       │
     └──────┬────────┘
            │ cache miss fetch
            v
     ┌───────────────┐
     │ Branch Cache  │
     │ (branch1)     │
     └──────┬────────┘
            │ upstream fetch on miss
            v
     ┌───────────────┐
     │ Root Cache    │
     │ (root1)       │
     └───────────────┘
```

### Scenario Intent

1. Write a key to `root1`.
2. Read the key from `leaf1`.
3. Confirm data is returned correctly at `leaf1`.
4. Confirm holder state converges to include `root1`, `branch1`, and `leaf1`.

### Suggested Scenario File

- `cache/tests/root_branch_leaf_scenarios.json`

### Candidate Scenarios

1. `root_branch_leaf_pullthrough_basic`
2. `root_branch_leaf_repeat_read_leaf_hit`
3. `root_branch_leaf_branch_eviction_then_refill`
4. `negative_leaf_read_missing_key_should_fail`
5. `negative_leaf_read_wrong_expected_value_should_fail`

### Validation Goals

- Leaf reads trigger branch/root fetch chain when key is initially absent downstream.
- Branch caches intermediate data so repeated leaf reads avoid unnecessary root fetches.
- Control plane holder map eventually includes all expected tiers for pulled keys.
- Negative cases fail deterministically for missing keys and value mismatches.

## Use Case 4: Root + Branch + Leaf + Child Leaf

Validate deeper-hierarchy pull-through behavior by adding one more cache tier below `leaf1` (`leaf2`).

### Topology Diagram

```text
┌───────────────────────┐
│ Test Framework Client │
└───────────┬───────────┘
            │ read/write (Redis)
            v
     ┌───────────────┐
     │ Child Leaf    │
     │ (leaf2)       │
     └──────┬────────┘
            │ cache miss fetch
            v
     ┌───────────────┐
     │ Leaf Cache    │
     │ (leaf1)       │
     └──────┬────────┘
            │ cache miss fetch
            v
     ┌───────────────┐
     │ Branch Cache  │
     │ (branch1)     │
     └──────┬────────┘
            │ upstream fetch on miss
            v
     ┌───────────────┐
     │ Root Cache    │
     │ (root1)       │
     └───────────────┘
```

### Scenario Intent

1. Write a key to `root1`.
2. Read from `branch1`, then `leaf1`, then `leaf2`.
3. Confirm reads return expected value at each tier.
4. Confirm holder state converges to include all four nodes for the pulled key.

### Scenario File

- `cache/tests/root_branch_leaf_child_scenarios.json`

### Included Scenarios

1. `root_branch_leaf_child_pullthrough_basic` (positive)
2. `root_branch_leaf_child_repeat_read_deep_leaf_hit` (positive)
3. `root_branch_leaf_child_deep_leaf_delete_then_refill` (positive)
4. `negative_child_leaf_read_missing_key_should_fail` (negative, expected failure)
5. `negative_child_leaf_read_wrong_expected_value_should_fail` (negative, expected failure)

### Negative Scenario Note

- Negative scenarios are intentionally expected to fail specific operations.
- They are asserted either with scenario-level `expected_failure: true` or step-level `write_expect_failure`, so full files can include both positive and negative scenarios.

### Validation Goals

- Pull-through continues to work with an additional downstream cache tier.
- Data remains readable and consistent at the deepest node (`leaf2`).
- Control plane mapping converges to reflect placement across all tiers.

## Use Case 5: Root + Two Leaf Nodes

Validate fan-out behavior where two leaf caches independently request data from the same root cache.

### Topology Diagram

```text
                   ┌───────────────┐
                   │ Root Cache    │
                   │ (root1)       │
                   └──────┬────────┘
                          │
             ┌────────────┴────────────┐
             │                         │
             v                         v
      ┌───────────────┐         ┌───────────────┐
      │ Leaf Cache    │         │ Leaf Cache    │
      │ (leaf1)       │         │ (leaf2)       │
      └──────┬────────┘         └──────┬────────┘
             │ read/write (Redis)      │ read/write (Redis)
             └────────────┬────────────┘
                          v
                ┌───────────────────────┐
                │ Test Framework Client │
                └───────────────────────┘
```

### Scenario Intent

1. Write a key to `root1`.
2. Read from `leaf1` and `leaf2`.
3. Confirm each leaf returns expected value.
4. Confirm holder state converges to include `root1`, `leaf1`, and `leaf2`.

### Scenario File

- `cache/tests/root_two_leaf_scenarios.json`

### Included Scenarios

1. `root_two_leaf_pullthrough_basic` (positive)
2. `root_two_leaf_reverse_read_order` (positive)
3. `root_two_leaf_delete_one_leaf_other_still_serves` (positive)
4. `negative_leaf1_read_missing_key_should_fail` (negative, expected failure)
5. `negative_leaf2_read_wrong_expected_value_should_fail` (negative, expected failure)

### Negative Scenario Note

- Negative scenarios are intentionally expected to fail specific operations.
- They are asserted either with scenario-level `expected_failure: true` or step-level `write_expect_failure`, so full files can include both positive and negative scenarios.

### Validation Goals

- Both leaves can independently populate from upstream and serve subsequent reads.
- Location mapping converges to include all expected holders after fan-out reads.
- Failure cases are deterministic for missing-key and wrong-expected-value checks.

## Use Case 6: Root + Two Branches + Two Leaf Nodes

Validate multi-path fan-out behavior with two branch paths under one root, each serving a dedicated leaf.

### Topology Diagram

```text
                      ┌───────────────┐
                      │ Root Cache    │
                      │ (root1)       │
                      └──────┬────────┘
                             │
               ┌─────────────┴─────────────┐
               │                           │
               v                           v
        ┌───────────────┐           ┌───────────────┐
        │ Branch Cache  │           │ Branch Cache  │
        │ (branch1)     │           │ (branch2)     │
        └──────┬────────┘           └──────┬────────┘
               │                           │
               v                           v
        ┌───────────────┐           ┌───────────────┐
        │ Leaf Cache    │           │ Leaf Cache    │
        │ (leaf1)       │           │ (leaf2)       │
        └───────────────┘           └───────────────┘
```

### Scenario Intent

1. Write data to `root1`.
2. Read through path A (`branch1` -> `leaf1`) and path B (`branch2` -> `leaf2`).
3. Confirm values are consistent across both paths.
4. Confirm control-plane location converges to all expected holders.

### Scenario File

- `cache/tests/root_two_branches_two_leaves_scenarios.json`

### Included Scenarios

1. `two_branches_two_leaves_dual_pullthrough_basic` (positive)
2. `two_branches_two_leaves_parallel_paths_independent_keys` (positive)
3. `two_branches_two_leaves_branch_delete_refill` (positive)
4. `negative_leaf1_missing_key_should_fail` (negative, expected failure)
5. `negative_leaf2_wrong_expected_value_should_fail` (negative, expected failure)

### Negative Scenario Note

- Negative scenarios are intentionally expected to fail specific operations.
- They are asserted either with scenario-level `expected_failure: true` or step-level `write_expect_failure`, so full files can include both positive and negative scenarios.

### Validation Goals

- Both branch/leaf paths independently pull data from root as needed.
- One path can refill after local deletion without breaking the other path.
- Holder mappings converge for each key according to the path(s) that accessed it.

## Use Case 7: Root + Two Branch Paths (3 Nodes Deep Each)

Validate deep multi-hop pull-through with two independent branch paths, each 3 nodes deep under the root.

### Topology Diagram

```text
                         ┌───────────────┐
                         │ Root Cache    │
                         │ (root1)       │
                         └──────┬────────┘
                                │
               ┌────────────────┴────────────────┐
               │                                 │
               v                                 v
        ┌───────────────┐                 ┌───────────────┐
        │ branch1a      │                 │ branch2a      │
        └──────┬────────┘                 └──────┬────────┘
               v                                 v
        ┌───────────────┐                 ┌───────────────┐
        │ branch1b      │                 │ branch2b      │
        └──────┬────────┘                 └──────┬────────┘
               v                                 v
        ┌───────────────┐                 ┌───────────────┐
        │ branch1c      │                 │ branch2c      │
        └───────────────┘                 └───────────────┘
```

### Scenario Intent

1. Write data to `root1`.
2. Read down branch path A (`branch1a` -> `branch1b` -> `branch1c`).
3. Read down branch path B (`branch2a` -> `branch2b` -> `branch2c`).
4. Confirm values remain consistent through all hops.
5. Confirm control-plane location converges to include expected holders.

### Scenario File

- `cache/tests/root_two_branches_three_deep_scenarios.json`

### Included Scenarios

1. `two_branches_three_deep_pullthrough_basic` (positive)
2. `two_branches_three_deep_independent_keys` (positive)
3. `two_branches_three_deep_midnode_delete_then_refill` (positive)
4. `negative_branch1c_missing_key_should_fail` (negative, expected failure)
5. `negative_branch2c_wrong_expected_value_should_fail` (negative, expected failure)
6. `negative_branch2c_write_when_full_noeviction_should_fail` (negative, expected failure)

### Negative Scenario Note

- Negative scenarios are intentionally expected to fail specific operations.
- They are asserted either with scenario-level `expected_failure: true` or step-level `write_expect_failure`, so full files can include both positive and negative scenarios.

### Validation Goals

- Deep path traversal behaves correctly across multiple intermediate caches.
- Both deep paths can be exercised independently without cross-path corruption.
- Holder mapping converges as keys propagate to deeper nodes.

## Use Case 8: Root + Three Leaf Nodes

Validate fan-out behavior where three leaf caches independently fetch data from a single root cache.

### Topology Diagram

```text
                        ┌───────────────┐
                        │ Root Cache    │
                        │ (root1)       │
                        └──────┬────────┘
                               │
             ┌─────────────────┼─────────────────┐
             │                 │                 │
             v                 v                 v
      ┌───────────────┐ ┌───────────────┐ ┌───────────────┐
      │ Leaf Cache    │ │ Leaf Cache    │ │ Leaf Cache    │
      │ (leaf1)       │ │ (leaf2)       │ │ (leaf3)       │
      └───────────────┘ └───────────────┘ └───────────────┘
```

### Scenario Intent

1. Write a key to `root1`.
2. Read from `leaf1`, `leaf2`, and `leaf3`.
3. Confirm each leaf returns the expected value.
4. Confirm control-plane location converges to include all four holders.

### Scenario File

- `cache/tests/root_three_leaf_scenarios.json`

### Included Scenarios

1. `root_three_leaf_pullthrough_basic` (positive)
2. `root_three_leaf_mixed_read_order` (positive)
3. `root_three_leaf_delete_one_leaf_others_serve` (positive)
4. `negative_leaf2_missing_key_should_fail` (negative, expected failure)
5. `negative_leaf3_wrong_expected_value_should_fail` (negative, expected failure)
6. `negative_leaf3_write_when_full_noeviction_should_fail` (negative, expected failure)

### Negative Scenario Note

- Negative scenarios are intentionally expected to fail specific operations.
- They are asserted either with scenario-level `expected_failure: true` or step-level `write_expect_failure`, so full files can include both positive and negative scenarios.

### Validation Goals

- All three leaves can independently populate from root.
- Deleting one leaf copy does not break reads from other leaf nodes.
- Holder mappings converge to the nodes that accessed each key.

## Use Case 9: Root + Three Branch Paths (3 Nodes Deep Each)

Validate deep multi-path pull-through behavior with three independent branch paths under one root, each path 3 nodes deep.

### Topology Diagram

```text
                               ┌───────────────┐
                               │ Root Cache    │
                               │ (root1)       │
                               └──────┬────────┘
                                      │
           ┌──────────────────────────┼──────────────────────────┐
           │                          │                          │
           v                          v                          v
     ┌───────────────┐          ┌───────────────┐          ┌───────────────┐
     │ branch1a      │          │ branch2a      │          │ branch3a      │
     └──────┬────────┘          └──────┬────────┘          └──────┬────────┘
            v                          v                          v
     ┌───────────────┐          ┌───────────────┐          ┌───────────────┐
     │ branch1b      │          │ branch2b      │          │ branch3b      │
     └──────┬────────┘          └──────┬────────┘          └──────┬────────┘
            v                          v                          v
     ┌───────────────┐          ┌───────────────┐          ┌───────────────┐
     │ branch1c      │          │ branch2c      │          │ branch3c      │
     └───────────────┘          └───────────────┘          └───────────────┘
```

### Scenario Intent

1. Write data to `root1`.
2. Read down each branch path (`a -> b -> c`) for all 3 branches.
3. Confirm values are consistent at each hop.
4. Confirm control-plane location converges to expected holders across paths.

### Scenario File

- `cache/tests/root_three_branches_three_deep_scenarios.json`

### Included Scenarios

1. `three_branches_three_deep_pullthrough_basic` (positive)
2. `three_branches_three_deep_independent_keys` (positive)
3. `three_branches_three_deep_midnode_delete_then_refill` (positive)
4. `negative_branch3c_missing_key_should_fail` (negative, expected failure)
5. `negative_branch2c_wrong_expected_value_should_fail` (negative, expected failure)
6. `negative_branch3c_write_when_full_noeviction_should_fail` (negative, expected failure)

### Negative Scenario Note

- Negative scenarios are intentionally expected to fail specific operations.
- They are asserted either with scenario-level `expected_failure: true` or step-level `write_expect_failure`, so full files can include both positive and negative scenarios.

### Validation Goals

- Each deep branch path can pull through independently without corrupting the others.
- Mid-path deletion/refill works while other paths continue serving.
- Holder mappings converge as data propagates to deeper nodes per path.
