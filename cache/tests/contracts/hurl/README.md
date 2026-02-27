# HAL Cache Control Plane Contract Tests (Hurl)

This directory contains a stateful contract test suite for the cache control-plane REST API defined in `/Volumes/My Shared Files/DevBox/hal/cache/openapi.yaml`.

## Coverage

The suite covers all documented endpoints:

- `GET /v1/instances`
- `GET /v1/instances/{id}`
- `POST /v1/instances/{id}/register`
- `POST /v1/instances/{id}/heartbeat`
- `DELETE /v1/instances/{id}/deregister`
- `GET /v1/content/locate/{key}`
- `GET /v1/content/nearest/{key}`
- `GET /v1/content/instances/{id}/keys`
- `POST /v1/content/instances/{id}/keys`
- `GET /v1/topology`
- `GET /v1/topology/proximity`
- `GET /v1/topology/routes/{from}/{to}`
- `POST /v1/events`
- `POST /v1/events/batch`

It includes positive and negative coverage, including:

- duplicate registration (`409`)
- invalid payload validation (`400`)
- missing resources (`404`)
- event ingestion for `key_added`, `key_updated`, and `key_evicted`
- batch ingestion and validation
- state convergence checks after events and deregistration

## Required Runtime

- Running cache control plane reachable at `HAL_CACHE_CONTROL_PLANE_URL` (defaults to `http://localhost:8080`)
- `hurl` CLI installed (`hurl 7+` recommended)

## Run

From repo root:

```bash
bash cache/tests/run-tests.sh --contract-only
```

Optional environment variables:

- `HAL_CACHE_CONTROL_PLANE_URL` (default `http://localhost:8080`)
- `HAL_CACHE_CONTRACT_RUN_ID` (default current epoch seconds)
- `HURL_BIN` (default `hurl`)

## Notes

- The suite uses unique instance IDs and keys per run (`run_id`) to avoid collisions.
- Event-related assertions use request-level retries to tolerate asynchronous state updates in the control plane.
