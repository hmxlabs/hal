#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
CONTROL_PLANE_DIR="${SCRIPT_DIR}/../control-plane"
CONTROL_PLANE_BIN="${CONTROL_PLANE_DIR}/bin/cache-control-plane"
CONTROL_PLANE_SCRIPT="${CONTROL_PLANE_DIR}/scripts/run-local.sh"
CONTRACT_SUITE_DIR="${SCRIPT_DIR}/contracts/hurl"
CONTRACT_SUITE_FILE="${CONTRACT_SUITE_DIR}/cache-control-plane-full-lifecycle.hurl"
LOG_DIR="${CONTRACT_SUITE_DIR}/logs"
CACHE_NODE_DIR="${SCRIPT_DIR}/../cache-node/src"
CACHE_NODE_BIN="${CACHE_NODE_DIR}/bin/cache-node"

BASE_URL="${HAL_CACHE_CONTROL_PLANE_URL:-http://localhost:8080}"
RUN_ID="${HAL_CACHE_CONTRACT_RUN_ID:-$(date +%s)}"
HURL_BIN="${HURL_BIN:-hurl}"
HURL_VERBOSE="${HAL_CACHE_HURL_VERBOSE:-1}"
CONTRACT_ONLY=false
SKIP_UNIT_TESTS=false
UNIT_TEST_ONLY=false
RUN_CACHE_SCENARIOS=false
RUN_CACHE_SCENARIOS_EXPLICIT=false
ROOT_NODE_SPEC=""
BRANCH_NODE_SPECS=()
LEAF_NODE_SPECS=()
SCENARIO_FILES=()
CONTRACT_LOG_FILE="${HAL_CACHE_CONTRACT_LOG_FILE:-${LOG_DIR}/contract-${RUN_ID}.log}"
SCENARIO_TIMEOUT_SECONDS="${HAL_CACHE_CACHE_SCENARIO_TIMEOUT_SECONDS:-10}"
AUTO_START_CONTROL_PLANE="${HAL_CACHE_AUTOSTART_CONTROL_PLANE:-1}"
AUTO_START_CACHE_NODES="${HAL_CACHE_AUTOSTART_CACHE_NODES:-1}"
CACHE_NODE_PIDS=()
CONTROL_PLANE_PID=""
CONTROL_PLANE_STARTED=false
CACHE_NODE_INSTANCE_IDS=()
WAIT_FOR_SERVICES_SECONDS="${HAL_CACHE_WAIT_FOR_SERVICES_SECONDS:-30}"
NODE_INFO=()
CONTRACT_TIMEOUT_SECONDS="${HAL_CACHE_CONTRACT_TIMEOUT_SECONDS:-120}"
CLEAN_CONTROL_PLANE_REDIS="${HAL_CACHE_CLEAN_CONTROL_PLANE_REDIS:-1}"
CONTROL_PLANE_LOG_FILE="${HAL_CACHE_CONTROL_PLANE_LOG_FILE:-/tmp/hal-cache-control-plane.log}"
CONTROL_PLANE_REDIS_PORT="${HAL_CACHE_REDIS_PORT:-6500}"
SCENARIO_COMMAND_TIMEOUT_SECONDS="${HAL_CACHE_SCENARIO_COMMAND_TIMEOUT_SECONDS:-120}"

declare -a DEFAULT_BRANCH_DEFS=(
  "branch1:6380:branch-a:branch:root1"
  "branch1a:6381:branch-a1:branch:branch1"
  "branch1b:6382:branch-a2:branch:branch1"
  "branch1c:6383:branch-a3:branch:branch1"
  "branch2a:6384:branch-b1:branch:root1"
  "branch2b:6385:branch-b2:branch:branch2a"
  "branch2c:6386:branch-b3:branch:branch2a"
  "branch3a:6387:branch-c1:branch:root1"
  "branch3b:6388:branch-c2:branch:branch3a"
  "branch3c:6389:branch-c3:branch:branch3a"
)

declare -a DEFAULT_LEAF_DEFS=(
  "leaf1:6390:leaf-a:leaf:branch1"
  "leaf2:6391:leaf-b:leaf:branch2a"
  "leaf3:6392:leaf-c:leaf:branch3a"
)

DEFAULT_ROOT_NODE="root1"
DEFAULT_ROOT_INSTANCE_PREFIX="root"
DEFAULT_ROOT_PORT="6379"

SCENARIO_DEFAULT_FILES=(
  "${SCRIPT_DIR}/single_root_scenarios.json"
  "${SCRIPT_DIR}/root_leaf_scenarios.json"
  "${SCRIPT_DIR}/root_two_leaf_scenarios.json"
  "${SCRIPT_DIR}/root_branch_leaf_scenarios.json"
  "${SCRIPT_DIR}/root_branch_leaf_child_scenarios.json"
  "${SCRIPT_DIR}/root_two_branches_two_leaves_scenarios.json"
  "${SCRIPT_DIR}/root_two_branches_three_deep_scenarios.json"
  "${SCRIPT_DIR}/root_three_leaf_scenarios.json"
  "${SCRIPT_DIR}/root_three_branches_three_deep_scenarios.json"
)

usage() {
  cat <<'USAGE'
Usage: cache/tests/run-tests.sh [options]

Options:
  --contract-only       Run contract tests (plus unit tests unless skipped)
  --base-url URL        Control-plane base URL (default: HAL_CACHE_CONTROL_PLANE_URL or http://localhost:8080)
  --run-id ID           Unique run suffix for instance IDs and keys (default: HAL_CACHE_CONTRACT_RUN_ID or epoch seconds)
  --hurl-bin PATH       Hurl binary to use (default: HURL_BIN or hurl)
  --hurl-log FILE       Write full Hurl output to FILE (default: contracts/hurl/logs/contract-\${RUN_ID}.log)
  --hurl-verbose BOOL   Enable hurl verbose output (default: 1; set HAL_CACHE_HURL_VERBOSE)
  --skip-unit-tests     Skip unit tests in this run
  --unit-only           Run only unit tests
  --cache-scenarios     Run cache topology scenarios (via python framework; default for non-contract-only runs)
  --no-cache-scenarios  Skip cache topology scenarios
  --root NODE_SPEC      Root node spec: name=host:port[;id=<cp-instance-id>]
  --branch NODE_SPEC    Branch node spec (repeatable): name=host:port[;id=<cp-instance-id>]
  --leaf NODE_SPEC      Leaf node spec (repeatable): name=host:port[;id=<cp-instance-id>]
  --no-autostart-control-plane  Do not auto-start local control plane
  --no-autostart-cache-nodes    Do not auto-start cache-node daemons
  --scenario-file FILE  Additional cache scenario file (repeatable)
  -h, --help            Show this help
USAGE
}

log() {
  printf '[%s] %s\n' "$(date +'%Y-%m-%dT%H:%M:%S%z')" "$*" >&2
}

hurl_supports_verbose() {
  "${HURL_BIN}" --help 2>&1 | grep -q -- "--verbose"
}

terminate_pid() {
  local pid=$1
  local name=$2
  local attempts=0

  if ! kill -0 "${pid}" >/dev/null 2>&1; then
    return 0
  fi

  if ! kill "${pid}" >/dev/null 2>&1; then
    return 0
  fi

  while (( attempts < 30 )); do
    if ! kill -0 "${pid}" >/dev/null 2>&1; then
      wait "${pid}" 2>/dev/null || true
      return 0
    fi
    sleep 0.25
    attempts=$((attempts + 1))
  done

  log "Warning: ${name} (pid ${pid}) did not stop after SIGTERM, sending SIGKILL"
  kill -9 "${pid}" >/dev/null 2>&1 || true
  wait "${pid}" 2>/dev/null || true
}

reset_control_plane_state() {
  if [[ "${CLEAN_CONTROL_PLANE_REDIS}" != "1" ]]; then
    return 0
  fi

  if ! command -v redis-cli >/dev/null 2>&1; then
    log "warning: redis-cli not found; skipping control-plane redis reset"
    return 0
  fi

  if ! redis-cli -p "${CONTROL_PLANE_REDIS_PORT}" ping >/dev/null 2>&1; then
    log "warning: redis-server on ${CONTROL_PLANE_REDIS_PORT} is not available; skipping control-plane redis reset"
    return 0
  fi

  if ! redis-cli -p "${CONTROL_PLANE_REDIS_PORT}" flushdb >/dev/null 2>&1; then
    log "warning: failed to flush Redis db on port ${CONTROL_PLANE_REDIS_PORT}; continuing"
    return 0
  fi
  log "Flushed Redis db on port ${CONTROL_PLANE_REDIS_PORT} before running tests"
}

run_with_timeout() {
  local timeout_seconds=$1
  shift
  local -a cmd=("$@")
  local pid
  local elapsed=0

  if [[ "${timeout_seconds}" -le 0 ]]; then
    "${cmd[@]}"
    return $?
  fi

  "${cmd[@]}" &
  pid=$!

  while kill -0 "${pid}" >/dev/null 2>&1; do
    if (( elapsed >= timeout_seconds )); then
      log "Timed out after ${timeout_seconds}s while running: ${cmd[*]}"
      terminate_pid "${pid}" "scenario command"
      return 124
    fi
    sleep 1
    elapsed=$((elapsed + 1))
  done

  wait "${pid}"
}

run_contract_suite() {
  local -a hurl_cmd
  local hurl_status=0

  log "Running cache control-plane contract suite"
  log "  base_url=${BASE_URL}"
  log "  run_id=${RUN_ID}"
  log "  contract_suite=${CONTRACT_SUITE_FILE}"
  log "  output_log=${CONTRACT_LOG_FILE}"
  log "  contract_timeout_seconds=${CONTRACT_TIMEOUT_SECONDS}"

  hurl_cmd=(
    "${HURL_BIN}"
    --test
    --location
    --max-time
    "${CONTRACT_TIMEOUT_SECONDS}"
    --progress-bar
    --variable
    "base_url=${BASE_URL}"
    --variable
    "run_id=${RUN_ID}"
    "${CONTRACT_SUITE_FILE}"
  )

  if [[ "${HURL_VERBOSE}" == "1" ]] && hurl_supports_verbose; then
    hurl_cmd+=(--verbose)
  fi

  if ! "${hurl_cmd[@]}" 2>&1 | tee "${CONTRACT_LOG_FILE}"; then
    hurl_status=$?
    log "Contract suite failed with exit code ${hurl_status}; see ${CONTRACT_LOG_FILE}"
    return "${hurl_status}"
  fi
}

node_name() {
  local spec=$1
  echo "${spec%%=*}"
}

node_host_port() {
  local spec=$1
  echo "${spec#*=}" | cut -d ';' -f 1
}

node_instance_id() {
  local spec=$1
  local node_host_and_opts
  if [[ "$spec" == *";id="* ]]; then
    node_host_and_opts="${spec#*;id=}"
    echo "${node_host_and_opts%%;*}"
    return 0
  fi
  echo ""
}

node_spec_with_port() {
  local spec_name=$1
  local port=$2
  local instance_id=$3
  echo "${spec_name}=127.0.0.1:${port};id=${instance_id}"
}

ensure_spec_has_id() {
  local spec=$1
  local fallback_id=$2
  local name
  local host_port
  local id

  name="$(node_name "${spec}")"
  host_port="$(node_host_port "${spec}")"
  id="$(node_instance_id "${spec}")"

  if [[ -z "${id}" ]]; then
    echo "${name}=${host_port};id=${fallback_id}"
    return 0
  fi
  echo "${spec}"
}

spec_contains_name() {
  local needle=$1
  local spec
  shift
  for spec in "$@"; do
    if [[ "$(node_name "$spec")" == "$needle" ]]; then
      return 0
    fi
  done
  return 1
}

find_spec_by_name() {
  local needle=$1
  local spec
  shift
  for spec in "$@"; do
    if [[ "$(node_name "$spec")" == "$needle" ]]; then
      echo "$spec"
      return 0
    fi
  done
  return 1
}

node_info_set() {
  local name=$1
  local tier=$2
  local parent=$3
  local id=$4
  local addr=$5
  local entry
  local existing=()
  local updated=0
  local existing_name
  local existing_tier
  local existing_parent
  local existing_id
  local existing_addr

  for entry in "${NODE_INFO[@]-}"; do
    IFS='|' read existing_name existing_tier existing_parent existing_id existing_addr <<EOF
${entry}
EOF
    if [[ "${existing_name}" == "${name}" ]]; then
      existing+=("${name}|${tier}|${parent}|${id}|${addr}")
      updated=1
    else
      existing+=("${entry}")
    fi
  done
  if [[ "${updated}" == "0" ]]; then
    existing+=("${name}|${tier}|${parent}|${id}|${addr}")
  fi
  NODE_INFO=("${existing[@]}")
}

node_info_get() {
  local name=$1
  local field=$2
  local entry
  local entry_name
  local entry_tier
  local entry_parent
  local entry_id
  local entry_addr

  for entry in "${NODE_INFO[@]-}"; do
    IFS='|' read entry_name entry_tier entry_parent entry_id entry_addr <<EOF
${entry}
EOF
    if [[ "${entry_name}" == "${name}" ]]; then
      case "${field}" in
        id) echo "${entry_id}" ;;
        tier) echo "${entry_tier}" ;;
        parent) echo "${entry_parent}" ;;
        addr) echo "${entry_addr}" ;;
        *) echo "" ;;
      esac
      return 0
    fi
  done
  echo ""
}

wait_for_http_ok() {
  local url=$1
  local timeout_seconds=$2
  local elapsed=0

  while (( elapsed < timeout_seconds )); do
    if command -v curl >/dev/null 2>&1 && \
      curl -sS "${url}" >/dev/null 2>&1; then
      return 0
    fi
    sleep 1
    elapsed=$((elapsed + 1))
  done

  return 1
}

wait_for_control_plane() {
  wait_for_http_ok "${BASE_URL%/}/v1/instances" "$1"
}

extract_base_url_port() {
  local url=$1
  local host_port

  host_port="${url#*://}"
  host_port="${host_port%%/*}"
  if [[ "${host_port}" == *:* ]]; then
    echo "${host_port##*:}"
  else
    echo "8080"
  fi
}

start_control_plane_if_needed() {
  if wait_for_control_plane 2; then
    log "Control plane already reachable at ${BASE_URL}"
    return 0
  fi

  if [[ "${AUTO_START_CONTROL_PLANE}" != "1" ]]; then
    return 1
  fi

  log "Starting local control plane via ${CONTROL_PLANE_SCRIPT}"
  log "  redis-port=${CONTROL_PLANE_REDIS_PORT}"
  if ! command -v "${CONTROL_PLANE_DIR}/bin/cache-control-plane" >/dev/null 2>&1; then
    make -C "${CONTROL_PLANE_DIR}" all
  fi
  if ! command -v curl >/dev/null 2>&1; then
    echo "error: curl is required for control-plane auto-start path" >&2
    return 1
  fi
  if ! command -v redis-server >/dev/null 2>&1; then
    echo "error: redis-server is required for control-plane auto-start path" >&2
    return 1
  fi

  HAL_CACHE_CONTROL_PLANE_PORT="$(extract_base_url_port "${BASE_URL}")" \
  HAL_CACHE_REDIS_PORT="${CONTROL_PLANE_REDIS_PORT}" \
  "${CONTROL_PLANE_SCRIPT}" >"${CONTROL_PLANE_LOG_FILE}" 2>&1 &
  CONTROL_PLANE_PID=$!
  CONTROL_PLANE_STARTED=true
  log "Control plane log file: ${CONTROL_PLANE_LOG_FILE}"
  if ! wait_for_control_plane "${WAIT_FOR_SERVICES_SECONDS}"; then
    echo "error: control plane failed to start" >&2
    return 1
  fi

  reset_control_plane_state
}

register_instance_with_control_plane() {
  local id=$1
  local address=$2
  local tier=$3
  local parent_id=$4
  local payload
  local response

  payload="{\"address\":\"${address}\",\"tier\":\"${tier}\",\"region\":\"test-${RUN_ID}\""
  if [[ -n "${parent_id}" ]]; then
    payload="${payload},\"parentId\":\"${parent_id}\""
  fi
  payload="${payload},\"maxSize\":0}"

  response=$(mktemp "${TMPDIR:-/tmp}/hal-cache-reg-XXXXXX.json")
  if ! status=$(curl -sS -o "${response}" -w '%{http_code}' \
      -X POST "${BASE_URL%/}/v1/instances/${id}/register" \
      -H "Content-Type: application/json" \
      -d "${payload}"); then
    echo "error: failed to register instance ${id} at ${BASE_URL}" >&2
    rm -f "${response}"
    return 1
  fi
  if [[ "${status}" != "201" && "${status}" != "409" ]]; then
    echo "error: instance registration failed for ${id} (status=${status}): $(cat "${response}")" >&2
    rm -f "${response}"
    return 1
  fi
  rm -f "${response}"
}

prepare_cache_specs() {
  local name
  local port
  local base_id
  local tier
  local parent
  local def
  local spec
  local root_name
  local next_branches=()
  local next_leaves=()
  local provided
  local default_id

  if [[ -z "${ROOT_NODE_SPEC}" ]]; then
    ROOT_NODE_SPEC="$(node_spec_with_port "${DEFAULT_ROOT_NODE}" "${DEFAULT_ROOT_PORT}" "${DEFAULT_ROOT_INSTANCE_PREFIX}-${RUN_ID}")"
  fi
  ROOT_NODE_SPEC="$(ensure_spec_has_id "${ROOT_NODE_SPEC}" "${DEFAULT_ROOT_INSTANCE_PREFIX}-${RUN_ID}")"
  root_name="$(node_name "${ROOT_NODE_SPEC}")"
  node_info_set "${root_name}" "root" "" "$(node_instance_id "${ROOT_NODE_SPEC}")" "$(node_host_port "${ROOT_NODE_SPEC}")"

  for def in "${DEFAULT_BRANCH_DEFS[@]}"; do
    IFS=':' read -r name port base_id tier parent <<< "${def}"
    default_id="${base_id}-${RUN_ID}"
    provided=""
    for spec in "${BRANCH_NODE_SPECS[@]-}"; do
      if [[ "$(node_name "${spec}")" == "${name}" ]]; then
        provided="${spec}"
        break
      fi
    done
    if [[ -z "${provided}" ]]; then
      provided="$(node_spec_with_port "${name}" "${port}" "${default_id}")"
      next_branches+=("${provided}")
      node_info_set "${name}" "${tier}" "${parent}" "${default_id}" "$(node_host_port "${provided}")"
    else
      provided="$(ensure_spec_has_id "${provided}" "${default_id}")"
      next_branches+=("${provided}")
      node_info_set "${name}" "${tier}" "${parent}" "$(node_instance_id "${provided}")" "$(node_host_port "${provided}")"
    fi
  done
  BRANCH_NODE_SPECS=("${next_branches[@]}")

  for def in "${DEFAULT_LEAF_DEFS[@]}"; do
    IFS=':' read -r name port base_id tier parent <<< "${def}"
    default_id="${base_id}-${RUN_ID}"
    provided=""
    for spec in "${LEAF_NODE_SPECS[@]-}"; do
      if [[ "$(node_name "${spec}")" == "${name}" ]]; then
        provided="${spec}"
        break
      fi
    done
    if [[ -z "${provided}" ]]; then
      provided="$(node_spec_with_port "${name}" "${port}" "${default_id}")"
      next_leaves+=("${provided}")
      node_info_set "${name}" "${tier}" "${parent}" "${default_id}" "$(node_host_port "${provided}")"
    else
      provided="$(ensure_spec_has_id "${provided}" "${default_id}")"
      next_leaves+=("${provided}")
      node_info_set "${name}" "${tier}" "${parent}" "$(node_instance_id "${provided}")" "$(node_host_port "${provided}")"
    fi
  done
  LEAF_NODE_SPECS=("${next_leaves[@]}")
}

finalize_instance_maps() {
  CACHE_NODE_INSTANCE_IDS=()
  if [[ -n "${ROOT_NODE_SPEC}" ]]; then
    CACHE_NODE_INSTANCE_IDS+=("$(node_instance_id "${ROOT_NODE_SPEC}")")
  fi
  for spec in "${BRANCH_NODE_SPECS[@]-}" "${LEAF_NODE_SPECS[@]-}"; do
    CACHE_NODE_INSTANCE_IDS+=("$(node_instance_id "${spec}")")
  done
}

start_node_processes() {
  local spec name host_port port address
  local id tier parent parent_id
  if [[ "${AUTO_START_CACHE_NODES}" != "1" ]]; then
    return 0
  fi

  if [[ ! -x "${CACHE_NODE_BIN}" ]]; then
    make -C "${CACHE_NODE_DIR}" all
  fi

  mkdir -p "${LOG_DIR}"
    for spec in "${ROOT_NODE_SPEC}" "${BRANCH_NODE_SPECS[@]-}" "${LEAF_NODE_SPECS[@]-}"; do
    name="$(node_name "${spec}")"
    host_port="$(node_host_port "${spec}")"
    id="$(node_instance_id "${spec}")"
    tier="$(node_info_get "${name}" tier)"
    parent="$(node_info_get "${name}" parent)"
    parent_id=""
    if [[ -n "${parent}" ]]; then
      parent_id="$(node_info_get "${parent}" id)"
    fi

    port="${host_port##*:}"
    address="${host_port}"
    if [[ -z "${tier}" ]]; then
      echo "warning: unknown tier for node ${name}; skipping start"
      continue
    fi
    if [[ -z "${id}" ]]; then
      echo "warning: missing instance-id for node ${name}; skipping start"
      continue
    fi

    if ! register_instance_with_control_plane "${id}" "${address}" "${tier}" "${parent_id}"; then
      echo "warning: failed to register instance ${id}; skipping start" >&2
      continue
    fi

    log "Starting cache-node ${name} (${id}) on ${host_port}"

    "${CACHE_NODE_BIN}" \
      --instance-id "${id}" \
      --listen-host 127.0.0.1 \
      --listen-port "${port}" \
      --control-plane-events-url "${BASE_URL%/}/v1/events" \
      >>"${LOG_DIR}/cache-node-${name}-${id}.log" 2>&1 &
    CACHE_NODE_PIDS+=("$!")
  done

  sleep 1
}

start_distributed_services() {
  prepare_cache_specs
  finalize_instance_maps
  start_control_plane_if_needed
  start_node_processes
}

cleanup_distributed_services() {
  local pid
  for pid in "${CACHE_NODE_PIDS[@]-}"; do
    if [[ -n "${pid}" ]]; then
      terminate_pid "${pid}" "cache-node"
    fi
  done
  if [[ "${CONTROL_PLANE_STARTED}" == "true" && -n "${CONTROL_PLANE_PID}" ]]; then
    terminate_pid "${CONTROL_PLANE_PID}" "cache-control-plane"
  fi
  for spec in "${CACHE_NODE_INSTANCE_IDS[@]-}"; do
    if [[ -n "${spec}" ]]; then
      curl -sS -X DELETE "${BASE_URL%/}/v1/instances/${spec}/deregister" >/dev/null 2>&1 || true
    fi
  done
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --contract-only)
      CONTRACT_ONLY=true
      shift
      ;;
    --skip-unit-tests)
      SKIP_UNIT_TESTS=true
      shift
      ;;
    --unit-only)
      UNIT_TEST_ONLY=true
      shift
      ;;
    --cache-scenarios)
      RUN_CACHE_SCENARIOS=true
      RUN_CACHE_SCENARIOS_EXPLICIT=true
      shift
      ;;
    --no-cache-scenarios)
      RUN_CACHE_SCENARIOS=false
      RUN_CACHE_SCENARIOS_EXPLICIT=true
      shift
      ;;
    --no-autostart-control-plane)
      AUTO_START_CONTROL_PLANE=0
      shift
      ;;
    --no-autostart-cache-nodes)
      AUTO_START_CACHE_NODES=0
      shift
      ;;
    --root)
      if [[ $# -lt 2 ]]; then
        echo "error: --root requires a value" >&2
        exit 2
      fi
      ROOT_NODE_SPEC="$2"
      shift 2
      ;;
    --branch)
      if [[ $# -lt 2 ]]; then
        echo "error: --branch requires a value" >&2
        exit 2
      fi
      BRANCH_NODE_SPECS+=("$2")
      shift 2
      ;;
    --leaf)
      if [[ $# -lt 2 ]]; then
        echo "error: --leaf requires a value" >&2
        exit 2
      fi
      LEAF_NODE_SPECS+=("$2")
      shift 2
      ;;
    --scenario-file)
      if [[ $# -lt 2 ]]; then
        echo "error: --scenario-file requires a value" >&2
        exit 2
      fi
      SCENARIO_FILES+=("$2")
      shift 2
      ;;
    --base-url)
      if [[ $# -lt 2 ]]; then
        echo "error: --base-url requires a value" >&2
        exit 2
      fi
      BASE_URL="$2"
      shift 2
      ;;
    --run-id)
      if [[ $# -lt 2 ]]; then
        echo "error: --run-id requires a value" >&2
        exit 2
      fi
      RUN_ID="$2"
      shift 2
      ;;
    --hurl-bin)
      if [[ $# -lt 2 ]]; then
        echo "error: --hurl-bin requires a value" >&2
        exit 2
      fi
      HURL_BIN="$2"
      shift 2
      ;;
    --hurl-log)
      if [[ $# -lt 2 ]]; then
        echo "error: --hurl-log requires a value" >&2
        exit 2
      fi
      CONTRACT_LOG_FILE="$2"
      shift 2
      ;;
    --hurl-verbose)
      if [[ $# -lt 2 ]]; then
        echo "error: --hurl-verbose requires a value" >&2
        exit 2
      fi
      HURL_VERBOSE="$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "error: unknown option: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

if [[ "${CONTRACT_ONLY}" != "true" && "${RUN_CACHE_SCENARIOS_EXPLICIT}" != "true" ]]; then
  RUN_CACHE_SCENARIOS=true
fi

run_unit_tests() {
  log "Running cache control-plane unit tests"
  make -C "${CONTROL_PLANE_DIR}" test
}

run_cache_scenario_suite() {
  if [[ -z "${ROOT_NODE_SPEC}" ]]; then
    echo "error: cache scenarios require --root and topology-specific --branch/--leaf specs." >&2
    exit 2
  fi

  local files=()
  local failures=0
  local file_status=0
  if [[ "${#SCENARIO_FILES[@]}" -eq 0 ]]; then
    files=("${SCENARIO_DEFAULT_FILES[@]}")
  else
    files=("${SCENARIO_FILES[@]}")
  fi

  for scenario_file in "${files[@]}"; do
    if [[ ! -f "${scenario_file}" ]]; then
      echo "error: Cache scenario file not found: ${scenario_file}" >&2
      exit 1
    fi

    log "Running cache topology scenarios: ${scenario_file}"
    log "  timeout_seconds=${SCENARIO_TIMEOUT_SECONDS}"
    log "  command_timeout_seconds=${SCENARIO_COMMAND_TIMEOUT_SECONDS}"
    local -a cmd=(
      python3
      "${SCRIPT_DIR}/hal_cache_test_framework.py"
      run-scenario
      --control-plane "${BASE_URL}" \
      --root "${ROOT_NODE_SPEC}" \
      --timeout-seconds "${SCENARIO_TIMEOUT_SECONDS}" \
      --file "${scenario_file}"
    )

    for branch in "${BRANCH_NODE_SPECS[@]-}"; do
      cmd+=(--branch "${branch}")
    done
    for leaf in "${LEAF_NODE_SPECS[@]-}"; do
      cmd+=(--leaf "${leaf}")
    done

    if run_with_timeout "${SCENARIO_COMMAND_TIMEOUT_SECONDS}" "${cmd[@]}"; then
      file_status=0
    else
      file_status=$?
      failures=$((failures + 1))
      log "cache topology scenario file failed (exit ${file_status}): ${scenario_file}"
    fi
  done

  if (( failures > 0 )); then
    log "cache topology scenario suite completed with ${failures} failed file(s)"
    return 1
  fi

  log "cache topology scenario suite completed successfully"
}

if ! command -v make >/dev/null 2>&1; then
  echo "error: make is required to run unit tests" >&2
  exit 127
fi

trap cleanup_distributed_services EXIT INT TERM

if [[ "${UNIT_TEST_ONLY}" == "true" ]]; then
  run_unit_tests
  exit 0
fi

if [[ "${CONTRACT_ONLY}" == "true" ]]; then
  if [[ "${SKIP_UNIT_TESTS}" == "false" ]]; then
    run_unit_tests
  fi
  if ! command -v "${HURL_BIN}" >/dev/null 2>&1; then
    echo "error: Hurl binary not found: ${HURL_BIN}" >&2
    echo "Install Hurl from https://hurl.dev/docs/installation.html" >&2
    exit 127
  fi
  if [[ ! -f "${CONTRACT_SUITE_FILE}" ]]; then
    echo "error: Contract suite file not found: ${CONTRACT_SUITE_FILE}" >&2
    exit 1
  fi
  mkdir -p "${LOG_DIR}"
  if ! start_control_plane_if_needed; then
    echo "error: control plane unavailable and could not be started" >&2
    exit 1
  fi
  run_contract_suite
  if [[ "${RUN_CACHE_SCENARIOS}" == "true" ]]; then
    if ! command -v python3 >/dev/null 2>&1; then
      echo "error: python3 is required for cache scenario tests" >&2
      exit 127
    fi
    start_distributed_services
    run_cache_scenario_suite
  fi
  exit 0
fi

if [[ "${SKIP_UNIT_TESTS}" == "false" ]]; then
  run_unit_tests
fi

if ! command -v "${HURL_BIN}" >/dev/null 2>&1; then
  echo "error: Hurl binary not found: ${HURL_BIN}" >&2
  echo "Install Hurl from https://hurl.dev/docs/installation.html" >&2
  exit 127
fi

if [[ ! -f "${CONTRACT_SUITE_FILE}" ]]; then
  echo "error: Contract suite file not found: ${CONTRACT_SUITE_FILE}" >&2
  exit 1
fi

mkdir -p "${LOG_DIR}"
if ! start_control_plane_if_needed; then
  echo "error: control plane unavailable and could not be started" >&2
  exit 1
fi

run_contract_suite
if [[ "${RUN_CACHE_SCENARIOS}" == "true" ]]; then
  if ! command -v python3 >/dev/null 2>&1; then
    echo "error: python3 is required for cache scenario tests" >&2
    exit 127
  fi
  start_distributed_services
  run_cache_scenario_suite
fi
