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

- Negative scenarios are intentionally expected to fail.
- Run them individually using `--scenario <name>` so one expected failure does not stop execution of other scenarios in the same file.

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

- Negative scenarios are intentionally expected to fail.
- Run them individually using `--scenario <name>` so one expected failure does not stop execution of other scenarios in the same file.

### Validation Goals

- Pull-through continues to work with an additional downstream cache tier.
- Data remains readable and consistent at the deepest node (`leaf2`).
- Control plane mapping converges to reflect placement across all tiers.
