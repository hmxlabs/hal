# Python Usage Scenario Runner (Option 2)

This runner executes **data-plane + control-plane** cache scenarios:

- Populate/query cache nodes using the Redis/Valkey API via `valkey-cli` or `redis-cli`
- Verify control-plane state via REST (`/v1/content/*`)

Source matrix:

- `/Volumes/My Shared Files/DevBox/hal/cache/cache-usage-test-matrix.md`

## Implemented scenarios

Implemented:

- `USE-001` to `USE-040`

## List scenarios

```bash
python3 /Volumes/My\ Shared\ Files/DevBox/hal/cache/tests/python/run_usage_tests.py --list --control-plane-url http://localhost:8080 --root-id r --root-host 127.0.0.1 --root-port 6379 --branch-id b --branch-host 127.0.0.1 --branch-port 6380 --leaf-id l --leaf-host 127.0.0.1 --leaf-port 6381
```

## Run implemented defaults

```bash
python3 /Volumes/My\ Shared\ Files/DevBox/hal/cache/tests/python/run_usage_tests.py \
  --control-plane-url http://localhost:8080 \
  --root-id root-us-east --root-host 127.0.0.1 --root-port 6379 \
  --branch-id branch-us-west --branch-host 127.0.0.1 --branch-port 6380 \
  --leaf-id leaf-us-west-1 --leaf-host 127.0.0.1 --leaf-port 6381
```

Tip: running all 40 scenarios by default is broad. For a quick pass, run only P0:

```bash
python3 /Volumes/My\ Shared\ Files/DevBox/hal/cache/tests/python/run_usage_tests.py \
  --control-plane-url http://localhost:8080 \
  --root-id root-us-east --root-host 127.0.0.1 --root-port 6379 \
  --branch-id branch-us-west --branch-host 127.0.0.1 --branch-port 6380 \
  --leaf-id leaf-us-west-1 --leaf-host 127.0.0.1 --leaf-port 6381 \
  --priority P0
```

## Run specific scenarios

```bash
python3 /Volumes/My\ Shared\ Files/DevBox/hal/cache/tests/python/run_usage_tests.py \
  --control-plane-url http://localhost:8080 \
  --root-id root-us-east --root-host 127.0.0.1 --root-port 6379 \
  --branch-id branch-us-west --branch-host 127.0.0.1 --branch-port 6380 \
  --leaf-id leaf-us-west-1 --leaf-host 127.0.0.1 --leaf-port 6381 \
  --sibling-leaf-id leaf-us-west-2 --sibling-leaf-host 127.0.0.1 --sibling-leaf-port 6382 \
  --scenarios USE-001,USE-006
```

## Notes

- The runner assumes cache nodes are already configured with hierarchy behavior.
- It does not mock the cache itself.
- For authenticated nodes, add `--redis-user` and `--redis-password`.
- Fault/restart mutation scenarios use optional hook commands:
  - `--source-down-cmd`, `--source-up-cmd`
  - `--intermediate-down-cmd`, `--intermediate-up-cmd`
  - `--control-plane-down-cmd`, `--control-plane-up-cmd`
  - `--control-plane-restart-cmd`
  - `--requester-restart-cmd`
- If hooks are omitted, the runner executes soft-mode fallbacks unless `--strict-hooks` is set.
