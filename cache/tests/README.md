# HAL Cache Test Framework

This directory contains:

- A Hurl-based REST contract test suite for the cache control plane API
- A Python CLI for validating cache-node behavior against the control plane

## Contract Tests (Hurl)

The Hurl suite validates all control-plane REST endpoints (positive and
negative), including state transitions for key add/update/evict flows and
deregistration cleanup.

Location:

- `cache/tests/contracts/hurl/cache-control-plane-full-lifecycle.hurl`

Run:

```bash
bash cache/tests/run-tests.sh --contract-only
```

You can override the control-plane URL with:

```bash
HAL_CACHE_CONTROL_PLANE_URL=http://localhost:8080 bash cache/tests/run-tests.sh --contract-only
```

## Scenario Tests (Python)

The Python CLI validates cache-node behavior against the HAL cache control
plane.

It writes/reads data directly on Redis-compatible cache nodes and polls the
control plane (`/v1/content/locate/{key}` and `/v1/instances/{id}`) to verify
that content placement and instance status converge correctly.

## Requirements

- Python 3.9+
- `redis` Python package (`pip install redis`)
- Running cache nodes (Redis-compatible / ValKey-based)
- Running HAL cache control plane

## Node Spec Format

Node specs are passed to `--root`, `--branch`, and `--leaf`:

- `name=host:port`
- `name=redis://host:port/0`
- `name=host:port;id=<control-plane-instance-id>` (recommended if address matching is ambiguous)

Examples:

- `root1=127.0.0.1:6379;id=root-a`
- `branch1=redis://127.0.0.1:6380/0;id=branch-a`
- `leaf1=127.0.0.1:6381`

## One-off Commands

Write a key to the root and verify the control plane reflects the holder:

```bash
python3 cache/tests/hal_cache_test_framework.py write-verify \
  --control-plane http://localhost:8080 \
  --root 'root1=127.0.0.1:6379;id=root-a' \
  --branch 'branch1=127.0.0.1:6380;id=branch-a' \
  --leaf 'leaf1=127.0.0.1:6381;id=leaf-a' \
  --node root1 \
  --key demo:key:1 \
  --value "hello"
```

Read the same key from a leaf and require a control-plane state change (leaf
must be newly listed as a holder after the read):

```bash
python3 cache/tests/hal_cache_test_framework.py read-verify \
  --control-plane http://localhost:8080 \
  --root 'root1=127.0.0.1:6379;id=root-a' \
  --branch 'branch1=127.0.0.1:6380;id=branch-a' \
  --leaf 'leaf1=127.0.0.1:6381;id=leaf-a' \
  --node leaf1 \
  --key demo:key:1 \
  --expected-value "hello" \
  --require-state-change
```

## Scenario File (JSON)

The CLI can run many scenarios via `run-scenario`.

Supported step `op` values:

- `write`
- `write_expect_failure`
- `read`
- `delete`
- `capture_config`
- `set_config`
- `assert_locate_contains`
- `sleep`

See `cache/tests/example_scenarios.json` for a starter file.

Run all scenarios in a file:

```bash
python3 cache/tests/hal_cache_test_framework.py run-scenario \
  --control-plane http://localhost:8080 \
  --root 'root1=127.0.0.1:6379;id=root-a' \
  --branch 'branch1=127.0.0.1:6380;id=branch-a' \
  --leaf 'leaf1=127.0.0.1:6381;id=leaf-a' \
  --file cache/tests/example_scenarios.json
```

Run one named scenario:

```bash
python3 cache/tests/hal_cache_test_framework.py run-scenario \
  --control-plane http://localhost:8080 \
  --root 'root1=127.0.0.1:6379;id=root-a' \
  --branch 'branch1=127.0.0.1:6380;id=branch-a' \
  --leaf 'leaf1=127.0.0.1:6381;id=leaf-a' \
  --file cache/tests/example_scenarios.json \
  --scenario leaf_pullthrough_basic
```

## Notes

- Control plane updates may be asynchronous, so the framework polls until a
  timeout (`--timeout-seconds`) before failing.
- For read verification, use `--require-state-change` (or scenario
  `require_state_change`) when the read is expected to populate a new holder.
- Scenario `expected_failure: true` is now honored by the runner: the scenario
  passes only when a `FrameworkError` is observed.
- Use scenario `expected_error_contains` to ensure expected-failure scenarios
  fail for the right reason.
- The framework currently validates control-plane holder mapping and instance
  status via documented endpoints, not event stream internals.
