# Cache Control Plane Tests (Hurl + Curl)

This test implementation is based on:

- `/Volumes/My Shared Files/DevBox/hal/cache/tests.md`
- `/Volumes/My Shared Files/DevBox/hal/cache/test-matrix.md`

Framework and tooling:

- `hurl` for HTTP request/response test flows
- `curl` + `jq` for supplemental checks (edge cases and performance loops)
- `bash` runner script

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
