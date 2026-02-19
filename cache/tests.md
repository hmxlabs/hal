# HAL Cache Control Plane Test Specification

## Source of truth

This specification is based on:

- `cache/openapi.yaml`

The cache control plane is tested as a REST API component. Non-REST interfaces are out of scope.

## Scope

The test suite validates:

- API contract compliance with OpenAPI 3.1
- Endpoint behavior for Instances, Content, Topology, and Events APIs
- Request validation and error semantics
- Stateful consistency across related endpoints
- Recovery and scale behavior for the control plane API

## Out of scope

- ValKey internals and PostgreSQL schema implementation details
- ValKey fork (data-plane cache node) command compatibility testing
- Any undocumented non-REST behavior

## Test levels

### 1. Contract tests

- Validate `cache/openapi.yaml` syntax and OpenAPI semantic correctness.
- Ensure generated server/client stubs match current operations and schemas.
- Verify every implemented route maps to one OpenAPI path and method.

### 2. Endpoint behavior tests

Run per endpoint with positive and negative cases:

- Required path/query/body fields
- Enum constraints
- Response status code and schema shape
- Filtering, pagination, and edge-case handling

### 3. Stateful integration tests

Validate cross-endpoint workflows where API state evolves:

- Register -> heartbeat -> inventory update -> content lookup -> events -> deregister
- Topology and routing responses after instance lifecycle changes

### 4. Resilience tests

- Control plane restart with persisted registry/topology
- Reconciliation behavior for key inventory after restart
- Event ingestion behavior during transient dependency disruption

### 5. Performance tests

- High-throughput event ingestion (`/v1/events` and `/v1/events/batch`)
- Query latency under scale for content and topology reads
- Stability of key lookup performance with large key cardinality

## Test environment

- One control plane service under test
- Backing ValKey and PostgreSQL services (test fixtures)
- Deterministic test data for instances, topology, and keys
- Isolated database/cache state per test run

## Canonical fixtures

Use these baseline instances in integration tests:

- `root-us-east` (`tier=root`)
- `branch-us-west` (`tier=branch`, `parentId=root-us-east`)
- `leaf-us-west-1` (`tier=leaf`, `parentId=branch-us-west`)

Use representative keys:

- `dataset:imagenet:v1`
- `dataset:llama2-tokenizer`
- `checkpoint:resnet50:epoch10`

## Contract assertions derived from OpenAPI

- Path param `id` uses pattern `^[a-zA-Z0-9_-]+$`.
- Instance status enum is `[active, inactive, unverified]`.
- Tier enum is `[root, branch, leaf]`.
- Event type enum is `[key_added, key_updated, key_evicted]`.
- `/v1/content/nearest/{key}` requires `sourceInstanceId` query param.
- Batch events must have `minItems: 1` and `maxItems: 10000`.
- Standard error responses must follow `Error` schema (`code`, `message` required).

## Endpoint coverage requirements

Required endpoint coverage:

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

## Required negative tests

At minimum, include:

- Invalid ID format for `{id}` path parameters
- Missing required query/body fields
- Unknown enum values
- Not-found behavior for unknown instance and unknown key
- Conflict on duplicate registration
- Batch size violations (`0` items and `>10000` items)

## Acceptance criteria

The component passes when:

- All endpoints satisfy documented contracts and status codes.
- Cross-endpoint state transitions are consistent.
- Error handling is stable and schema-compliant.
- Resilience tests meet data-consistency expectations.
- Performance tests meet agreed SLOs.

## Deliverables

- Automated test suite grouped by contract, endpoint, integration, resilience, and performance categories
- Executable CI job producing test report artifacts
- This specification plus `cache/test-matrix.md` as the control-plane API case matrix
- `cache/cache-usage-test-matrix.md` as the end-to-end cache behavior and propagation matrix
