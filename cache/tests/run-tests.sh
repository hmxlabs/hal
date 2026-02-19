#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
HURL_DIR="${SCRIPT_DIR}/hurl"
OPENAPI_FILE="${ROOT_DIR}/cache/openapi.yaml"

BASE_URL="${CACHE_API_BASE_URL:-}"
INSECURE=0
WITH_PERF=0
VERBOSE=0
CONTRACT_ONLY=0
RESTART_CMD="${CACHE_API_RESTART_CMD:-}"
RECOVERY_TIMEOUT="${CACHE_RECOVERY_TIMEOUT:-60}"
PERF_BATCH_SIZE="${CACHE_PERF_BATCH_SIZE:-100}"
PERF_ROUNDS="${CACHE_PERF_ROUNDS:-20}"
PERF_MIN_EVENTS_PER_SEC="${CACHE_PERF_MIN_EVENTS_PER_SEC:-100}"
PERF_LOOKUP_ITERATIONS="${CACHE_PERF_LOOKUP_ITERATIONS:-100}"
PERF_NEAREST_P95_MS="${CACHE_PERF_NEAREST_P95_MS:-200}"
PERF_ROUTE_ITERATIONS="${CACHE_PERF_ROUTE_ITERATIONS:-100}"
PERF_ROUTE_P95_MS="${CACHE_PERF_ROUTE_P95_MS:-250}"

usage() {
  cat <<'EOF'
Usage: bash cache/tests/run-tests.sh [options]

Options:
  --base-url URL           Cache control plane base URL, e.g. http://localhost:8080
  --insecure               Skip TLS verification for HTTPS endpoints
  --with-perf              Run performance checks (PERF-001/002/003)
  --with-resilience CMD    Run resilience checks and use CMD to restart control plane
  --recovery-timeout SEC   Wait timeout after restart (default: 60)
  --contract-only          Run only OpenAPI contract checks (CT-001/002/003)
  --verbose                Verbose hurl output
  -h, --help               Show this help

Environment overrides:
  CACHE_API_BASE_URL
  CACHE_PERF_BATCH_SIZE
  CACHE_PERF_ROUNDS
  CACHE_PERF_MIN_EVENTS_PER_SEC
  CACHE_PERF_LOOKUP_ITERATIONS
  CACHE_PERF_NEAREST_P95_MS
  CACHE_PERF_ROUTE_ITERATIONS
  CACHE_PERF_ROUTE_P95_MS
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --base-url)
      [[ $# -ge 2 ]] || { echo "error: --base-url requires a value" >&2; exit 1; }
      BASE_URL="$2"
      shift 2
      ;;
    --insecure)
      INSECURE=1
      shift
      ;;
    --with-perf)
      WITH_PERF=1
      shift
      ;;
    --with-resilience)
      [[ $# -ge 2 ]] || { echo "error: --with-resilience requires a command" >&2; exit 1; }
      RESTART_CMD="$2"
      shift 2
      ;;
    --recovery-timeout)
      [[ $# -ge 2 ]] || { echo "error: --recovery-timeout requires a value" >&2; exit 1; }
      RECOVERY_TIMEOUT="$2"
      shift 2
      ;;
    --contract-only)
      CONTRACT_ONLY=1
      shift
      ;;
    --verbose)
      VERBOSE=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "error: unknown option: $1" >&2
      usage >&2
      exit 1
      ;;
  esac
done

require_cmd() {
  local cmd="$1"
  if ! command -v "$cmd" >/dev/null 2>&1; then
    echo "error: required command not found: $cmd" >&2
    exit 1
  fi
}

require_cmd hurl
require_cmd curl
require_cmd jq

TEST_PREFIX="cachetest_$(date +%s)_${RANDOM}"

HURL_COMMON_ARGS=(--test --jobs 1 --variable "base_url=${BASE_URL}" --variable "test_prefix=${TEST_PREFIX}")
if [[ "$INSECURE" -eq 1 ]]; then
  HURL_COMMON_ARGS+=(-k)
fi
if [[ "$VERBOSE" -eq 1 ]]; then
  HURL_COMMON_ARGS+=(--verbose)
fi

fail() {
  echo "error: $*" >&2
  exit 1
}

pass() {
  echo "ok: $*"
}

run_contract_checks() {
  echo "Running contract checks (CT-001/CT-002/CT-003)..."

  [[ -f "$OPENAPI_FILE" ]] || fail "openapi file not found: $OPENAPI_FILE"

  local openapi_version
  openapi_version="$(awk '/^openapi:/ {print $2; exit}' "$OPENAPI_FILE")"
  [[ "$openapi_version" == "3.1.0" ]] || fail "expected openapi: 3.1.0, got: ${openapi_version:-<missing>}"
  pass "CT-001 openapi version is 3.1.0"

  local dup_ops
  dup_ops="$(grep -E '^[[:space:]]*operationId:' "$OPENAPI_FILE" | awk '{print $2}' | sort | uniq -d || true)"
  [[ -z "$dup_ops" ]] || fail "duplicate operationId values found: $dup_ops"
  pass "CT-002 operationId values are unique"

  local error_block
  error_block="$(grep -A12 '^    Error:$' "$OPENAPI_FILE" || true)"
  [[ -n "$error_block" ]] || fail "CT-003 missing Error schema block"
  echo "$error_block" | grep -qE '^[[:space:]]*- code$' || fail "CT-003 Error.required missing code"
  echo "$error_block" | grep -qE '^[[:space:]]*- message$' || fail "CT-003 Error.required missing message"
  pass "CT-003 Error schema requires code and message"
}

curl_json() {
  local method="$1"
  local url="$2"
  local data_file="${3:-}"
  local body_file
  body_file="$(mktemp)"
  local code

  if [[ -n "$data_file" ]]; then
    if [[ "$INSECURE" -eq 1 ]]; then
      code="$(curl -sS -k -o "$body_file" -w '%{http_code}' -X "$method" -H 'Content-Type: application/json' --data-binary "@$data_file" "$url")"
    else
      code="$(curl -sS -o "$body_file" -w '%{http_code}' -X "$method" -H 'Content-Type: application/json' --data-binary "@$data_file" "$url")"
    fi
  else
    if [[ "$INSECURE" -eq 1 ]]; then
      code="$(curl -sS -k -o "$body_file" -w '%{http_code}' -X "$method" "$url")"
    else
      code="$(curl -sS -o "$body_file" -w '%{http_code}' -X "$method" "$url")"
    fi
  fi

  echo "$code|$body_file"
}

assert_error_schema_file() {
  local file="$1"
  jq -e '.code and .message' "$file" >/dev/null
}

run_hurl_suite() {
  echo "Running Hurl endpoint/integration suite with test prefix: ${TEST_PREFIX}"
  hurl "${HURL_COMMON_ARGS[@]}" "${HURL_DIR}/01-instances.hurl"
  hurl "${HURL_COMMON_ARGS[@]}" "${HURL_DIR}/02-content.hurl"
  hurl "${HURL_COMMON_ARGS[@]}" "${HURL_DIR}/03-topology.hurl"
  hurl "${HURL_COMMON_ARGS[@]}" "${HURL_DIR}/04-events.hurl"
  hurl "${HURL_COMMON_ARGS[@]}" "${HURL_DIR}/05-integration.hurl"
}

run_curl_supplemental_checks() {
  echo "Running supplemental curl checks (VAL-001, EVT-005, INT-003)..."

  # VAL-001
  local val_result val_code val_body
  val_result="$(curl_json GET "${BASE_URL}/v1/instances/bad\$id")"
  val_code="${val_result%%|*}"
  val_body="${val_result#*|}"
  if [[ "$val_code" != "400" && "$val_code" != "404" ]]; then
    fail "VAL-001 expected 400/404, got ${val_code}"
  fi
  if [[ -s "$val_body" ]]; then
    assert_error_schema_file "$val_body" || fail "VAL-001 response does not match Error schema"
  fi
  pass "VAL-001 invalid instance id rejected"
  rm -f "$val_body"

  # EVT-005 (oversize batch)
  local evt_id evt_payload evt_reg evt_reg_code evt_reg_body evt_post evt_post_code evt_post_body evt_del evt_del_code evt_del_body
  evt_id="${TEST_PREFIX}_evt_oversize"

  evt_payload="$(mktemp)"
  jq -n --arg id "$evt_id" --arg ts "2026-01-01T00:00:00Z" '
    {
      instanceId: $id,
      events: [range(0;10001) | {eventType: "key_added", timestamp: $ts, key: ("k" + (.|tostring))}]
    }
  ' > "$evt_payload"

  evt_reg_payload="$(mktemp)"
  jq -n --arg addr "127.0.0.1:22379" '{address:$addr, region:"us-west", tier:"root", maxSize:1073741824}' > "$evt_reg_payload"
  evt_reg="$(curl_json POST "${BASE_URL}/v1/instances/${evt_id}/register" "$evt_reg_payload")"
  evt_reg_code="${evt_reg%%|*}"
  evt_reg_body="${evt_reg#*|}"
  [[ "$evt_reg_code" == "201" || "$evt_reg_code" == "409" ]] || fail "EVT-005 setup register failed with ${evt_reg_code}"
  rm -f "$evt_reg_body" "$evt_reg_payload"

  evt_post="$(curl_json POST "${BASE_URL}/v1/events/batch" "$evt_payload")"
  evt_post_code="${evt_post%%|*}"
  evt_post_body="${evt_post#*|}"
  [[ "$evt_post_code" == "400" ]] || fail "EVT-005 expected 400 for oversize batch, got ${evt_post_code}"
  assert_error_schema_file "$evt_post_body" || fail "EVT-005 response does not match Error schema"
  pass "EVT-005 oversize batch rejected"
  rm -f "$evt_post_body" "$evt_payload"

  evt_del="$(curl_json DELETE "${BASE_URL}/v1/instances/${evt_id}/deregister")"
  evt_del_code="${evt_del%%|*}"
  evt_del_body="${evt_del#*|}"
  [[ "$evt_del_code" == "204" || "$evt_del_code" == "404" ]] || fail "EVT-005 cleanup failed with ${evt_del_code}"
  rm -f "$evt_del_body"

  # INT-003
  local root_id branch_id leaf_id
  root_id="${TEST_PREFIX}_int3_root"
  branch_id="${TEST_PREFIX}_int3_branch"
  leaf_id="${TEST_PREFIX}_int3_leaf"

  local root_payload branch_payload leaf_payload
  root_payload="$(mktemp)"
  branch_payload="$(mktemp)"
  leaf_payload="$(mktemp)"
  jq -n '{address:"127.0.0.1:23379", region:"us-west", tier:"root", maxSize:1073741824}' > "$root_payload"
  jq -n --arg parent "$root_id" '{address:"127.0.0.1:23380", region:"us-west", tier:"branch", parentId:$parent, maxSize:536870912}' > "$branch_payload"
  jq -n --arg parent "$branch_id" '{address:"127.0.0.1:23381", region:"us-west", tier:"leaf", parentId:$parent, maxSize:268435456}' > "$leaf_payload"

  local r1 r2 r3 c1 c2 c3 b1 b2 b3
  r1="$(curl_json POST "${BASE_URL}/v1/instances/${root_id}/register" "$root_payload")"; c1="${r1%%|*}"; b1="${r1#*|}"
  r2="$(curl_json POST "${BASE_URL}/v1/instances/${branch_id}/register" "$branch_payload")"; c2="${r2%%|*}"; b2="${r2#*|}"
  r3="$(curl_json POST "${BASE_URL}/v1/instances/${leaf_id}/register" "$leaf_payload")"; c3="${r3%%|*}"; b3="${r3#*|}"
  [[ "$c1" == "201" || "$c1" == "409" ]] || fail "INT-003 root register failed: $c1"
  [[ "$c2" == "201" || "$c2" == "409" ]] || fail "INT-003 branch register failed: $c2"
  [[ "$c3" == "201" || "$c3" == "409" ]] || fail "INT-003 leaf register failed: $c3"
  rm -f "$b1" "$b2" "$b3" "$root_payload" "$branch_payload" "$leaf_payload"

  local key_payload key_result key_code key_body
  key_payload="$(mktemp)"
  jq -n '{mode:"partial", keys:[{key:"dataset_int3", size:1024, lastAccessed:"2026-01-01T00:00:00Z"}]}' > "$key_payload"
  key_result="$(curl_json POST "${BASE_URL}/v1/content/instances/${leaf_id}/keys" "$key_payload")"
  key_code="${key_result%%|*}"; key_body="${key_result#*|}"
  [[ "$key_code" == "200" ]] || fail "INT-003 key update failed: $key_code"
  rm -f "$key_body" "$key_payload"

  local locate_before before_code before_body
  locate_before="$(curl_json GET "${BASE_URL}/v1/content/locate/dataset_int3")"
  before_code="${locate_before%%|*}"; before_body="${locate_before#*|}"
  [[ "$before_code" == "200" ]] || fail "INT-003 locate before failed: $before_code"
  jq -e --arg id "$leaf_id" '.instances | map(.instanceId) | index($id) != null' "$before_body" >/dev/null || fail "INT-003 leaf not present before deregister"
  rm -f "$before_body"

  local del_leaf del_leaf_code del_leaf_body
  del_leaf="$(curl_json DELETE "${BASE_URL}/v1/instances/${leaf_id}/deregister")"
  del_leaf_code="${del_leaf%%|*}"; del_leaf_body="${del_leaf#*|}"
  [[ "$del_leaf_code" == "204" ]] || fail "INT-003 leaf deregister failed: $del_leaf_code"
  rm -f "$del_leaf_body"

  local locate_after after_code after_body
  locate_after="$(curl_json GET "${BASE_URL}/v1/content/locate/dataset_int3")"
  after_code="${locate_after%%|*}"; after_body="${locate_after#*|}"
  [[ "$after_code" == "200" ]] || fail "INT-003 locate after failed: $after_code"
  jq -e --arg id "$leaf_id" '.instances | map(.instanceId) | index($id) == null' "$after_body" >/dev/null || fail "INT-003 leaf still present after deregister"
  rm -f "$after_body"

  local cleanup_item cleanup_code cleanup_body
  cleanup_item="$(curl_json DELETE "${BASE_URL}/v1/instances/${branch_id}/deregister")"; cleanup_code="${cleanup_item%%|*}"; cleanup_body="${cleanup_item#*|}"; [[ "$cleanup_code" == "204" || "$cleanup_code" == "404" ]] || fail "INT-003 cleanup branch failed"; rm -f "$cleanup_body"
  cleanup_item="$(curl_json DELETE "${BASE_URL}/v1/instances/${root_id}/deregister")"; cleanup_code="${cleanup_item%%|*}"; cleanup_body="${cleanup_item#*|}"; [[ "$cleanup_code" == "204" || "$cleanup_code" == "404" ]] || fail "INT-003 cleanup root failed"; rm -f "$cleanup_body"

  pass "INT-003 deregister removes key availability"
}

wait_for_api() {
  local deadline
  deadline=$(( $(date +%s) + RECOVERY_TIMEOUT ))

  while [[ $(date +%s) -le "$deadline" ]]; do
    local status
    if [[ "$INSECURE" -eq 1 ]]; then
      status="$(curl -sS -k -o /dev/null -w '%{http_code}' "${BASE_URL}/v1/instances" || true)"
    else
      status="$(curl -sS -o /dev/null -w '%{http_code}' "${BASE_URL}/v1/instances" || true)"
    fi
    if [[ "$status" == "200" ]]; then
      return 0
    fi
    sleep 2
  done
  return 1
}

run_resilience_checks() {
  [[ -n "$RESTART_CMD" ]] || fail "--with-resilience requires a restart command"
  echo "Running resilience checks (RES-001/RES-002)..."

  hurl "${HURL_COMMON_ARGS[@]}" "${HURL_DIR}/06-resilience-setup.hurl"

  echo "Restarting control plane with: ${RESTART_CMD}"
  eval "$RESTART_CMD"

  wait_for_api || fail "API did not recover within ${RECOVERY_TIMEOUT}s"
  pass "control plane recovered"

  hurl "${HURL_COMMON_ARGS[@]}" "${HURL_DIR}/07-resilience-verify.hurl"
}

p95_from_sorted_file_ms() {
  local file="$1"
  awk '
    NR > 0 { a[NR] = $1 }
    END {
      if (NR == 0) { print 0; exit }
      idx = int((0.95 * (NR - 1)) + 0.999999)
      if (idx < 1) idx = 1
      if (idx > NR) idx = NR
      print a[idx]
    }
  ' "$file"
}

run_performance_checks() {
  echo "Running performance checks (PERF-001/PERF-002/PERF-003)..."

  local perf_id
  perf_id="${TEST_PREFIX}_perf_root"

  local reg_payload reg_result reg_code reg_body
  reg_payload="$(mktemp)"
  jq -n '{address:"127.0.0.1:24379", region:"us-west", tier:"root", maxSize:1073741824}' > "$reg_payload"
  reg_result="$(curl_json POST "${BASE_URL}/v1/instances/${perf_id}/register" "$reg_payload")"
  reg_code="${reg_result%%|*}"; reg_body="${reg_result#*|}"
  [[ "$reg_code" == "201" || "$reg_code" == "409" ]] || fail "PERF setup register failed: $reg_code"
  rm -f "$reg_body" "$reg_payload"

  # PERF-001
  local batch_payload
  batch_payload="$(mktemp)"
  jq -n --arg id "$perf_id" --arg ts "2026-01-01T00:00:00Z" --argjson n "$PERF_BATCH_SIZE" '
    {
      instanceId: $id,
      events: [range(0; $n) | {eventType: "key_added", timestamp: $ts, key: ("perf_key_" + (.|tostring)), size: (. + 1)}]
    }
  ' > "$batch_payload"

  local start_ts end_ts elapsed total_events eps i post_result post_code post_body
  start_ts="$(python3 -c 'import time; print(time.time())')"
  i=0
  while [[ "$i" -lt "$PERF_ROUNDS" ]]; do
    post_result="$(curl_json POST "${BASE_URL}/v1/events/batch" "$batch_payload")"
    post_code="${post_result%%|*}"; post_body="${post_result#*|}"
    [[ "$post_code" == "202" ]] || fail "PERF-001 batch post failed: $post_code"
    jq -e --argjson n "$PERF_BATCH_SIZE" '.count == $n' "$post_body" >/dev/null || fail "PERF-001 unexpected batch count"
    rm -f "$post_body"
    i=$((i + 1))
  done
  end_ts="$(python3 -c 'import time; print(time.time())')"

  elapsed="$(awk -v s="$start_ts" -v e="$end_ts" 'BEGIN{print (e-s)}')"
  total_events=$((PERF_BATCH_SIZE * PERF_ROUNDS))
  eps="$(awk -v total="$total_events" -v sec="$elapsed" 'BEGIN{ if (sec <= 0) sec=0.000001; print (total/sec)}')"
  awk -v actual="$eps" -v min="$PERF_MIN_EVENTS_PER_SEC" 'BEGIN{ exit !(actual >= min) }' || fail "PERF-001 expected >= ${PERF_MIN_EVENTS_PER_SEC} events/s, got ${eps}"
  pass "PERF-001 throughput ${eps} events/s"
  rm -f "$batch_payload"

  # PERF-002 and PERF-003 setup hierarchy
  local root_id branch_id leaf_id
  root_id="${TEST_PREFIX}_perf_root2"
  branch_id="${TEST_PREFIX}_perf_branch2"
  leaf_id="${TEST_PREFIX}_perf_leaf2"

  local p1 p2 p3 r1 r2 r3 c1 c2 c3 b1 b2 b3
  p1="$(mktemp)"; p2="$(mktemp)"; p3="$(mktemp)"
  jq -n '{address:"127.0.0.1:24380", region:"us-west", tier:"root", maxSize:1073741824}' > "$p1"
  jq -n --arg parent "$root_id" '{address:"127.0.0.1:24381", region:"us-west", tier:"branch", parentId:$parent, maxSize:536870912}' > "$p2"
  jq -n --arg parent "$branch_id" '{address:"127.0.0.1:24382", region:"us-west", tier:"leaf", parentId:$parent, maxSize:268435456}' > "$p3"
  r1="$(curl_json POST "${BASE_URL}/v1/instances/${root_id}/register" "$p1")"; c1="${r1%%|*}"; b1="${r1#*|}"
  r2="$(curl_json POST "${BASE_URL}/v1/instances/${branch_id}/register" "$p2")"; c2="${r2%%|*}"; b2="${r2#*|}"
  r3="$(curl_json POST "${BASE_URL}/v1/instances/${leaf_id}/register" "$p3")"; c3="${r3%%|*}"; b3="${r3#*|}"
  [[ "$c1" == "201" || "$c1" == "409" ]] || fail "PERF hierarchy root failed: $c1"
  [[ "$c2" == "201" || "$c2" == "409" ]] || fail "PERF hierarchy branch failed: $c2"
  [[ "$c3" == "201" || "$c3" == "409" ]] || fail "PERF hierarchy leaf failed: $c3"
  rm -f "$b1" "$b2" "$b3" "$p1" "$p2" "$p3"

  local key_payload key_result key_code key_body
  key_payload="$(mktemp)"
  jq -n '{mode:"partial", keys:[{key:"dataset_perf", size:2048, lastAccessed:"2026-01-01T00:00:00Z"}]}' > "$key_payload"
  key_result="$(curl_json POST "${BASE_URL}/v1/content/instances/${root_id}/keys" "$key_payload")"
  key_code="${key_result%%|*}"; key_body="${key_result#*|}"
  [[ "$key_code" == "200" ]] || fail "PERF key seed failed: $key_code"
  rm -f "$key_body" "$key_payload"

  # PERF-002 nearest p95
  local nearest_samples nearest_i nearest_resp nearest_code nearest_body nearest_time
  nearest_samples="$(mktemp)"
  nearest_i=0
  while [[ "$nearest_i" -lt "$PERF_LOOKUP_ITERATIONS" ]]; do
    nearest_body="$(mktemp)"
    if [[ "$INSECURE" -eq 1 ]]; then
      nearest_resp="$(curl -sS -k -o "$nearest_body" -w '%{http_code} %{time_total}' "${BASE_URL}/v1/content/nearest/dataset_perf?sourceInstanceId=${leaf_id}")"
    else
      nearest_resp="$(curl -sS -o "$nearest_body" -w '%{http_code} %{time_total}' "${BASE_URL}/v1/content/nearest/dataset_perf?sourceInstanceId=${leaf_id}")"
    fi
    nearest_code="$(echo "$nearest_resp" | awk '{print $1}')"
    nearest_time="$(echo "$nearest_resp" | awk '{print $2}')"
    [[ "$nearest_code" == "200" ]] || fail "PERF-002 nearest lookup failed: $nearest_code"
    awk -v t="$nearest_time" 'BEGIN{printf "%.6f\n", t*1000.0}' >> "$nearest_samples"
    rm -f "$nearest_body"
    nearest_i=$((nearest_i + 1))
  done

  local p95_nearest
  p95_nearest="$(sort -n "$nearest_samples" | p95_from_sorted_file_ms /dev/stdin)"
  awk -v actual="$p95_nearest" -v max="$PERF_NEAREST_P95_MS" 'BEGIN{ exit !(actual <= max) }' || fail "PERF-002 expected p95 <= ${PERF_NEAREST_P95_MS}ms, got ${p95_nearest}ms"
  pass "PERF-002 nearest p95 ${p95_nearest}ms"
  rm -f "$nearest_samples"

  # PERF-003 route p95
  local route_samples route_i route_resp route_code route_body route_time
  route_samples="$(mktemp)"
  route_i=0
  while [[ "$route_i" -lt "$PERF_ROUTE_ITERATIONS" ]]; do
    route_body="$(mktemp)"
    if [[ "$INSECURE" -eq 1 ]]; then
      route_resp="$(curl -sS -k -o "$route_body" -w '%{http_code} %{time_total}' "${BASE_URL}/v1/topology/routes/${root_id}/${leaf_id}")"
    else
      route_resp="$(curl -sS -o "$route_body" -w '%{http_code} %{time_total}' "${BASE_URL}/v1/topology/routes/${root_id}/${leaf_id}")"
    fi
    route_code="$(echo "$route_resp" | awk '{print $1}')"
    route_time="$(echo "$route_resp" | awk '{print $2}')"
    [[ "$route_code" == "200" ]] || fail "PERF-003 route lookup failed: $route_code"
    awk -v t="$route_time" 'BEGIN{printf "%.6f\n", t*1000.0}' >> "$route_samples"
    rm -f "$route_body"
    route_i=$((route_i + 1))
  done

  local p95_route
  p95_route="$(sort -n "$route_samples" | p95_from_sorted_file_ms /dev/stdin)"
  awk -v actual="$p95_route" -v max="$PERF_ROUTE_P95_MS" 'BEGIN{ exit !(actual <= max) }' || fail "PERF-003 expected p95 <= ${PERF_ROUTE_P95_MS}ms, got ${p95_route}ms"
  pass "PERF-003 route p95 ${p95_route}ms"
  rm -f "$route_samples"

  # cleanup
  local del_item del_code del_body
  del_item="$(curl_json DELETE "${BASE_URL}/v1/instances/${leaf_id}/deregister")"; del_code="${del_item%%|*}"; del_body="${del_item#*|}"; rm -f "$del_body"
  del_item="$(curl_json DELETE "${BASE_URL}/v1/instances/${branch_id}/deregister")"; del_code="${del_item%%|*}"; del_body="${del_item#*|}"; rm -f "$del_body"
  del_item="$(curl_json DELETE "${BASE_URL}/v1/instances/${root_id}/deregister")"; del_code="${del_item%%|*}"; del_body="${del_item#*|}"; rm -f "$del_body"

  del_item="$(curl_json DELETE "${BASE_URL}/v1/instances/${perf_id}/deregister")"; del_code="${del_item%%|*}"; del_body="${del_item#*|}"; rm -f "$del_body"
}

run_contract_checks

if [[ "$CONTRACT_ONLY" -eq 1 ]]; then
  echo "Contract-only run complete"
  exit 0
fi

[[ -n "$BASE_URL" ]] || fail "--base-url (or CACHE_API_BASE_URL) is required for API tests"

echo "Using base URL: ${BASE_URL}"
echo "Using test prefix: ${TEST_PREFIX}"

run_hurl_suite
run_curl_supplemental_checks

if [[ -n "$RESTART_CMD" ]]; then
  run_resilience_checks
fi

if [[ "$WITH_PERF" -eq 1 ]]; then
  run_performance_checks
fi

echo "All requested tests passed"
