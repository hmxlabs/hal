# HAL Cache Control Plane (C + Redis)

Redis-backed implementation of the HAL cache control plane API.

## What It Implements

The service implements the contract-tested REST API in `cache/openapi.yaml`:

- Instance registration/list/detail/heartbeat/deregister
- Content locate/nearest/list keys/update keys
- Topology graph/proximity matrix/routes
- Event ingestion (`/v1/events`, `/v1/events/batch`)

All mutable control-plane state is persisted in Redis.

## Dependencies

Runtime/build dependencies:

- `redis` (server + CLI)
- `hiredis`
- `jansson`
- `libmicrohttpd`
- `pkgconf` (`pkg-config`)
- `hurl` (for contract tests)

Installed with Homebrew during development:

- Direct installs: `hiredis`, `jansson`, `libmicrohttpd`, `redis`, `pkgconf`
- Transitive installs pulled by Homebrew: `gettext`, `gnutls`, `libevent`, `libidn2`, `libtasn1`, `libunistring`, `nettle`, `p11-kit`, `unbound`

## Build

```bash
bash cache/control-plane/scripts/build.sh
```

Or:

```bash
make -C cache/control-plane all
```

## Run Locally

```bash
bash cache/control-plane/scripts/run-local.sh
```

Environment variables:

- `HAL_CACHE_CONTROL_PLANE_PORT` (default `8080`)
- `HAL_CACHE_REDIS_HOST` (default `127.0.0.1`)
- `HAL_CACHE_REDIS_PORT` (default `6379`)
- `HAL_CACHE_REDIS_DB` (default `0`)

You can also pass CLI flags directly:

```bash
cache/control-plane/bin/cache-control-plane \
  --port 8080 \
  --redis-host 127.0.0.1 \
  --redis-port 6379 \
  --redis-db 0
```

## Quality Checks

Compiler quality gate (strict warnings + syntax check):

```bash
make -C cache/control-plane check
```

`checkpatch.pl` is run when available. If unavailable on the host, the target still enforces strict compiler checks and reports that kernel `checkpatch.pl` is not present.

## Contract Tests

1. Start Redis and the control plane
2. Run:

```bash
HAL_CACHE_CONTROL_PLANE_URL=http://localhost:8080 \
  bash cache/tests/run-tests.sh --contract-only
```

## Notes

- The service is designed for contract compatibility and keeps topology/proximity computations simple and deterministic.
- Event ingestion updates key-holder state immediately in Redis.
