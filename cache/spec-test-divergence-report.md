# Cache Spec vs Test Implementation Divergence Report

## Scope reviewed

- Spec sources:
  - `cache/openapi.yaml`
  - `cache/tests.md`
  - `cache/test-matrix.md`
  - `cache/cache-usage-test-matrix.md`
- Test implementations:
  - `cache/tests/run-tests.sh`
  - `cache/tests/hurl/*.hurl`
  - `cache/tests/python/run_usage_tests.py`
  - `cache/tests/python/scenario_catalog.py`

## Summary

- Matrix ID coverage is complete for both control-plane (`CT/INS/VAL/CNT/TOP/EVT/INT/RES/PERF`) and usage (`USE-001..USE-040`).
- Divergences are mostly in **assertion depth** and **fidelity** (tests exist but do not validate everything the specs require).

## Divergences

| Area | Spec expectation | Implementation behavior | Divergence |
|---|---|---|---|
| Contract validation depth | OpenAPI should be parsed/linted for syntax + semantic correctness (`tests.md:31`, `test-matrix.md:13`) | `CT-001` only checks `openapi: 3.1.0` string (`tests/run-tests.sh:131-135`) | No real OpenAPI parse/lint validation. |
| Contract/stub parity | Ensure generated server/client stubs match operations/schemas (`tests.md:32`) | No stub generation/check in runner or suites (`tests/run-tests.sh`) | Required contract gate is missing. |
| Route-to-spec mapping | Verify every implemented route maps to exactly one OpenAPI path/method (`tests.md:33`) | No automated route inventory vs spec mapping | Required contract gate is missing. |
| Endpoint schema-shape validation | Endpoint tests should validate response schema shape (`tests.md:41`) | Many tests only assert a few fields exist; e.g. `INS-005` expects `CacheInstanceDetail` (`test-matrix.md:20`) but checks only `id` and `status` (`tests/hurl/01-instances.hurl:55-59`) | Schema compliance is only partially verified. |
| Filtering semantics | Validate filtering behavior (`tests.md:42`), including `INS-008/009` expectations (`test-matrix.md:23-24`) | Status/region tests only check `.instances` exists (`tests/hurl/01-instances.hurl:73-81`) | Filter correctness is not asserted. |
| Pagination semantics | `CNT-004` expects cursor/limit behavior, page size <= limit (`test-matrix.md:33`) | Only `?limit=1` call with existence checks (`tests/hurl/02-content.hurl:95-99`) | Limit and cursor semantics are not asserted. |
| Proximity ordering/filter assertions | `CNT-006` requires ordering by proximity (`test-matrix.md:35`); `TOP-003` expects matrix filtered to requested instances (`test-matrix.md:41`) | Tests only check field existence (`tests/hurl/02-content.hurl:107-110`, `tests/hurl/03-topology.hurl:47-51`) | Behavioral assertions are too weak. |
| Required negative coverage breadth | Required negatives include missing required query/body fields and invalid IDs (`tests.md:117-119`) | Missing-query checked only for nearest (`tests/hurl/02-content.hurl:118-122`); invalid ID tested only on `GET /v1/instances/{id}` (`tests/run-tests.sh:191-203`) | Negative coverage is narrower than spec intent across endpoints. |
| Canonical fixture usage | Integration should use canonical fixtures (`tests.md:72-82`, `test-matrix.md:50`) | Hurl/integration tests use generated IDs (`tests/run-tests.sh:107`) and different key forms such as `dataset_imagenet_v1`/`checkpoint_resnet50_epoch10` (`tests/hurl/02-content.hurl:41-47`, `63-64`) vs canonical `dataset:imagenet:v1`/`checkpoint:resnet50:epoch10` (`tests.md:80-82`) | Canonical fixture names and representative key values are not used literally. |
| Resilience scope | Include event ingestion behavior during transient dependency disruption (`tests.md:55`) | Resilience flow only covers restart + post-restart verification (`tests/run-tests.sh:316-329`, `tests/hurl/06-resilience-setup.hurl`, `tests/hurl/07-resilience-verify.hurl`) | Dependency-disruption ingestion scenario is not implemented. |
| Performance scope | Test high-throughput ingestion for `/v1/events` and `/v1/events/batch` (`tests.md:59`) plus large-cardinality lookup stability (`tests.md:61`) | PERF throughput targets only `/v1/events/batch` (`tests/run-tests.sh:359-387`); nearest latency uses one seeded key (`tests/run-tests.sh:408-439`) | Single-event throughput and large-cardinality stability are not covered. |
| CI/report deliverable | Executable CI job with test report artifacts (`tests.md:137`) | No CI workflow/artifact config was found in this repository snapshot during file scan | Deliverable is not evidenced in-repo. |

## Usage-matrix-specific divergences (Option 2 Python runner)

| Area | Usage matrix expectation | Implementation behavior | Divergence |
|---|---|---|---|
| CP observability side effects | Matrix emphasizes `/v1/content/*`, `/v1/events*`, `/v1/topology/*` assertions (`cache-usage-test-matrix.md:9`) | Runner can submit events but verifies mainly via `nearest/locate/keys/route` (`tests/python/run_usage_tests.py:177-214`, `267-310`). The OpenAPI defines event ingestion endpoints but no event-read endpoint (`openapi.yaml:386-438`). | Event-stream assertions in usage scenarios are only indirectly validated. |
| Event-focused CP assertions | Many scenarios require explicit event assertions (e.g., `USE-001/002/003/015/029` in `cache-usage-test-matrix.md:35-40`, `49`, `63`) | Corresponding scenario code does not validate event records/count/ordering in CP (examples: `use_001` `366-376`, `use_002` `379-387`, `use_003` `390-410`, `use_015` `633-643`, `use_029` `921-946`) | Scenario intent vs implemented assertions diverges. |
| Topology-shape fidelity | `USE-035` expects explicit `root->leaf` topology and route check (`cache-usage-test-matrix.md:69`) | Runner always requires root+branch+leaf args (`tests/python/run_usage_tests.py:1203-1213`), and `use_035` only checks leaf read value (`1063-1071`) | Two-tier topology expectation is not directly exercised/asserted. |
| Deeper-topology variant | `USE-036` expects deeper topology variant with extra branch level (`cache-usage-test-matrix.md:70`) | `use_036` validates route shape only for configured root->leaf path (`1074-1087`); runner model supports only root/branch/leaf(+optional sibling) | Deeper topology variant is not represented. |
| Failure-mutation strictness | Fault/restart scenarios imply actual mutation of infra state (`cache-usage-test-matrix.md:52-57`) | Hook commands are optional; when absent runner uses soft fallbacks (`tests/python/run_usage_tests.py:312-317`, README note `tests/python/README.md:62-68`) | Mutation fidelity depends on external hooks and may degrade silently. |

## What is aligned

- All control-plane matrix IDs are present in implementation references.
- All usage scenario IDs `USE-001..USE-040` exist in catalog and executors (`tests/python/scenario_catalog.py`, `tests/python/run_usage_tests.py:1154-1195`).
