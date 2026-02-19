# Cache Test Implementation

Implemented test suites derived from `cache/tests.md` and `cache/test-matrix.md`.

## Files

- `cache/tests/contract_test.rb`
- `cache/tests/endpoint_test.rb`
- `cache/tests/integration_test.rb`
- `cache/tests/resilience_test.rb`
- `cache/tests/performance_test.rb`
- `cache/tests/test_helper.rb`
- `cache/tests/all_tests.rb`

## Run

```bash
ruby cache/tests/all_tests.rb
```

Or use the runner script:

```bash
bash cache/tests/run-tests.sh
```

## Environment variables

- `CACHE_API_BASE_URL`: base URL for API-backed tests (for example, `http://localhost:8080`).
- `CACHE_API_INSECURE=1`: disable TLS verification for self-signed HTTPS endpoints.
- `CACHE_HTTP_OPEN_TIMEOUT`: optional open timeout seconds (default `5`).
- `CACHE_HTTP_READ_TIMEOUT`: optional read timeout seconds (default `20`).

Resilience tests:

- `CACHE_API_RESTART_CMD`: shell command to restart control plane process/service.
- `CACHE_RECOVERY_TIMEOUT`: seconds to wait for API recovery (default `60`).

Performance tests (opt-in):

- `CACHE_ENABLE_PERF=1`
- `CACHE_PERF_BATCH_SIZE` (default `100`)
- `CACHE_PERF_ROUNDS` (default `20`)
- `CACHE_PERF_MIN_EVENTS_PER_SEC` (default `100.0`)
- `CACHE_PERF_LOOKUP_ITERATIONS` (default `100`)
- `CACHE_PERF_NEAREST_P95_MS` (default `200.0`)
- `CACHE_PERF_ROUTE_ITERATIONS` (default `100`)
- `CACHE_PERF_ROUTE_P95_MS` (default `250.0`)
