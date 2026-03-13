#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
CONTROL_PLANE_DIR="${SCRIPT_DIR}/../control-plane"
CONTRACT_SUITE_DIR="${SCRIPT_DIR}/contracts/hurl"
CONTRACT_SUITE_FILE="${CONTRACT_SUITE_DIR}/cache-control-plane-full-lifecycle.hurl"
LOG_DIR="${CONTRACT_SUITE_DIR}/logs"

BASE_URL="${HAL_CACHE_CONTROL_PLANE_URL:-http://localhost:8080}"
RUN_ID="${HAL_CACHE_CONTRACT_RUN_ID:-$(date +%s)}"
HURL_BIN="${HURL_BIN:-hurl}"
HURL_VERBOSE="${HAL_CACHE_HURL_VERBOSE:-1}"
CONTRACT_ONLY=false
SKIP_UNIT_TESTS=false
UNIT_TEST_ONLY=false
RUN_CACHE_SCENARIOS=false
ROOT_NODE_SPEC=""
BRANCH_NODE_SPECS=()
LEAF_NODE_SPECS=()
SCENARIO_FILES=()
CONTRACT_LOG_FILE="${HAL_CACHE_CONTRACT_LOG_FILE:-${LOG_DIR}/contract-${RUN_ID}.log}"
SCENARIO_TIMEOUT_SECONDS="${HAL_CACHE_CACHE_SCENARIO_TIMEOUT_SECONDS:-10}"

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
  --cache-scenarios     Run cache topology scenarios (via python framework)
  --root NODE_SPEC      Root node spec: name=host:port[;id=<cp-instance-id>]
  --branch NODE_SPEC    Branch node spec (repeatable): name=host:port[;id=<cp-instance-id>]
  --leaf NODE_SPEC      Leaf node spec (repeatable): name=host:port[;id=<cp-instance-id>]
  --scenario-file FILE  Additional cache scenario file (repeatable)
  -h, --help            Show this help
USAGE
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

run_unit_tests() {
  echo "Running cache control-plane unit tests"
  make -C "${CONTROL_PLANE_DIR}" test
}

run_cache_scenario_suite() {
  if [[ -z "${ROOT_NODE_SPEC}" ]]; then
    echo "error: cache scenarios require --root and topology-specific --branch/--leaf specs." >&2
    exit 2
  fi

  local files=()
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

    echo "Running cache topology scenarios: ${scenario_file}"
    local -a cmd=(
      python3
      "${SCRIPT_DIR}/hal_cache_test_framework.py"
      run-scenario
      --control-plane "${BASE_URL}" \
      --root "${ROOT_NODE_SPEC}" \
      --timeout-seconds "${SCENARIO_TIMEOUT_SECONDS}" \
      --file "${scenario_file}"
    )

    for branch in "${BRANCH_NODE_SPECS[@]}"; do
      cmd+=(--branch "${branch}")
    done
    for leaf in "${LEAF_NODE_SPECS[@]}"; do
      cmd+=(--leaf "${leaf}")
    done

    "${cmd[@]}"
  done
}

if ! command -v make >/dev/null 2>&1; then
  echo "error: make is required to run unit tests" >&2
  exit 127
fi

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
  run_contract_suite() {
    echo "Running cache control-plane contract suite"
    echo "  base_url=${BASE_URL}"
    echo "  run_id=${RUN_ID}"
    echo "  contract_suite=${CONTRACT_SUITE_FILE}"
    echo "  output_log=${CONTRACT_LOG_FILE}"

    if [[ "${HURL_VERBOSE}" == "1" ]] && hurl_supports_verbose; then
      "${HURL_BIN}" \
        --test \
        --location \
        --verbose \
        --variable "base_url=${BASE_URL}" \
        --variable "run_id=${RUN_ID}" \
        "${CONTRACT_SUITE_FILE}" \
        2>&1 | tee "${CONTRACT_LOG_FILE}"
    else
      "${HURL_BIN}" \
        --test \
        --location \
        --variable "base_url=${BASE_URL}" \
        --variable "run_id=${RUN_ID}" \
        "${CONTRACT_SUITE_FILE}" \
        2>&1 | tee "${CONTRACT_LOG_FILE}"
    fi
  }
  hurl_supports_verbose() {
    "${HURL_BIN}" --help 2>&1 | grep -q -- "--verbose"
  }
  mkdir -p "${LOG_DIR}"
  run_contract_suite
  if [[ "${RUN_CACHE_SCENARIOS}" == "true" ]]; then
    if ! command -v python3 >/dev/null 2>&1; then
      echo "error: python3 is required for cache scenario tests" >&2
      exit 127
    fi
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

hurl_supports_verbose() {
  "${HURL_BIN}" --help 2>&1 | grep -q -- "--verbose"
}

run_contract_suite() {
  echo "Running cache control-plane contract suite"
  echo "  base_url=${BASE_URL}"
  echo "  run_id=${RUN_ID}"
  echo "  contract_suite=${CONTRACT_SUITE_FILE}"
  echo "  output_log=${CONTRACT_LOG_FILE}"

  if [[ "${HURL_VERBOSE}" == "1" ]] && hurl_supports_verbose; then
    "${HURL_BIN}" \
      --test \
      --location \
      --verbose \
      --variable "base_url=${BASE_URL}" \
      --variable "run_id=${RUN_ID}" \
      "${CONTRACT_SUITE_FILE}" \
      2>&1 | tee "${CONTRACT_LOG_FILE}"
  else
    "${HURL_BIN}" \
      --test \
      --location \
      --variable "base_url=${BASE_URL}" \
      --variable "run_id=${RUN_ID}" \
      "${CONTRACT_SUITE_FILE}" \
      2>&1 | tee "${CONTRACT_LOG_FILE}"
  fi
}

run_contract_suite
if [[ "${RUN_CACHE_SCENARIOS}" == "true" ]]; then
  if ! command -v python3 >/dev/null 2>&1; then
    echo "error: python3 is required for cache scenario tests" >&2
    exit 127
  fi
  run_cache_scenario_suite
fi
