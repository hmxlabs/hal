# Cache Tests

This test implementation is based on:

- `/Volumes/My Shared Files/DevBox/hal/cache/tests.md`
- `/Volumes/My Shared Files/DevBox/hal/cache/test-matrix.md`

Control-plane API framework and tooling:

- `hurl` for HTTP request/response test flows
- `curl` + `jq` for supplemental checks (edge cases and performance loops)
- `bash` runner script

Usage-scenario framework (Option 2):

- Python runner that uses `valkey-cli` or `redis-cli` for cache data-plane operations
- REST calls to the control plane for propagation/state assertions
- See `/Volumes/My Shared Files/DevBox/hal/cache/tests/python/README.md`

## Test files

Hurl suites:

- `/Volumes/My Shared Files/DevBox/hal/cache/tests/hurl/01-instances.hurl`
- `/Volumes/My Shared Files/DevBox/hal/cache/tests/hurl/02-content.hurl`
- `/Volumes/My Shared Files/DevBox/hal/cache/tests/hurl/03-topology.hurl`
- `/Volumes/My Shared Files/DevBox/hal/cache/tests/hurl/04-events.hurl`
- `/Volumes/My Shared Files/DevBox/hal/cache/tests/hurl/05-integration.hurl`
- `/Volumes/My Shared Files/DevBox/hal/cache/tests/hurl/06-resilience-setup.hurl`
- `/Volumes/My Shared Files/DevBox/hal/cache/tests/hurl/07-resilience-verify.hurl`

Runner:

- `/Volumes/My Shared Files/DevBox/hal/cache/tests/run-tests.sh`
- `/Volumes/My Shared Files/DevBox/hal/cache/tests/python/run_usage_tests.py`

## Run

Contract checks only:

```bash
bash /Volumes/My\ Shared\ Files/DevBox/hal/cache/tests/run-tests.sh --contract-only
```

Endpoint + integration + supplemental checks:

```bash
bash /Volumes/My\ Shared\ Files/DevBox/hal/cache/tests/run-tests.sh --base-url http://localhost:8080
```

Include resilience checks:

```bash
bash /Volumes/My\ Shared\ Files/DevBox/hal/cache/tests/run-tests.sh \
  --base-url http://localhost:8080 \
  --with-resilience "docker compose restart cache-control-plane"
```

Include performance checks:

```bash
bash /Volumes/My\ Shared\ Files/DevBox/hal/cache/tests/run-tests.sh \
  --base-url http://localhost:8080 \
  --with-perf
```

## Notes

- The runner generates a unique test prefix for every run to avoid ID collisions.
- Resilience tests run only when `--with-resilience` is provided.
- Performance tests run only when `--with-perf` is provided.
