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
     └──────┬────────┘
            │ state/lookup checks (REST)
            v
   ┌───────────────────┐
   │ Control Plane API │
   └───────────────────┘
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
