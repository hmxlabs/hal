#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
PROJECT_ROOT="$(cd -- "${ROOT_DIR}/.." && pwd)"
TESTS_DIR="${PROJECT_ROOT}/tests"
CONTRACT_SUITE_FILE="${TESTS_DIR}/contracts/hurl/cache-control-plane-full-lifecycle.hurl"

REDIS_HOST="${HAL_CACHE_REDIS_HOST:-127.0.0.1}"
REDIS_PORT="${HAL_CACHE_REDIS_PORT:-6379}"
REDIS_DB="${HAL_CACHE_REDIS_DB:-0}"
CONTROL_PLANE_PORT="${HAL_CACHE_CONTROL_PLANE_PORT:-8080}"
CONTROL_PLANE_URL="${HAL_CACHE_CONTROL_PLANE_URL:-http://127.0.0.1:${CONTROL_PLANE_PORT}}"
HURL_BIN="${HURL_BIN:-hurl}"

printf 'Building cache control plane\n'
make -C "${ROOT_DIR}" -B all

printf 'Running compiler quality checks\n'
make -C "${ROOT_DIR}" check

printf 'Preparing contract test dependencies\n'
if ! command -v redis-server >/dev/null 2>&1; then
  echo "error: redis-server not found in PATH" >&2
  exit 127
fi
if ! command -v redis-cli >/dev/null 2>&1; then
  echo "error: redis-cli not found in PATH" >&2
  exit 127
fi

if ! command -v curl >/dev/null 2>&1; then
  echo "error: curl not found in PATH" >&2
  exit 127
fi

if ! command -v ${HURL_BIN} >/dev/null 2>&1; then
  echo "warning: HURL_BIN '${HURL_BIN}' not found; contract tests may fail" >&2
fi

if [[ ! -f "${CONTRACT_SUITE_FILE}" ]]; then
  echo "error: contract suite not found: ${CONTRACT_SUITE_FILE}" >&2
  exit 1
fi

REDIS_STARTED=0
if ! redis-cli -h "${REDIS_HOST}" -p "${REDIS_PORT}" ping >/dev/null 2>&1; then
  redis-server --port "${REDIS_PORT}" --save "" --appendonly no --daemonize yes >/dev/null
  REDIS_STARTED=1
  echo "Started redis-server at ${REDIS_HOST}:${REDIS_PORT}"
fi

CONSOLE_LOG="/tmp/hal-cp-console-$$.log"
: > "${CONSOLE_LOG}"
SERVER_PID=""
cleanup() {
  if [ -n "${SERVER_PID}" ]; then
    kill "${SERVER_PID}" >/dev/null 2>&1 || true
    wait "${SERVER_PID}" >/dev/null 2>&1 || true
  fi
  if [ -f "${CONSOLE_LOG}" ]; then
    echo "--- cache-control-plane output log ---"
    cat "${CONSOLE_LOG}"
  fi
  if [ "${REDIS_STARTED}" -eq 1 ]; then
    redis-cli -h "${REDIS_HOST}" -p "${REDIS_PORT}" shutdown nosave >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT INT TERM

"${ROOT_DIR}/bin/cache-control-plane" \
  --port "${CONTROL_PLANE_PORT}" \
  --redis-host "${REDIS_HOST}" \
  --redis-port "${REDIS_PORT}" \
  --redis-db "${REDIS_DB}" \
  >"${CONSOLE_LOG}" 2>&1 &
SERVER_PID=$!

echo "Started cache-control-plane pid=${SERVER_PID} port=${CONTROL_PLANE_PORT}"

for _ in $(seq 1 30); do
  if curl -fsS "${CONTROL_PLANE_URL}/v1/instances" >/dev/null 2>&1; then
    break
  fi
  sleep 0.2
  
  if ! kill -0 "${SERVER_PID}" >/dev/null 2>&1; then
    echo "error: control-plane process exited before becoming healthy" >&2
    echo "--- control-plane log ---"
    cat "${CONSOLE_LOG}"
    exit 1
  fi

  if [ "${_}" -eq 30 ]; then
    echo "error: control-plane did not become ready" >&2
    echo "--- control-plane log ---"
    cat "${CONSOLE_LOG}"
    exit 1
  fi
done

echo "Running cache control-plane contract tests"
echo "  contract_suite=${CONTRACT_SUITE_FILE}"
HAL_CACHE_CONTROL_PLANE_URL="${CONTROL_PLANE_URL}" \
  HAL_CACHE_CONTROL_PLANE_VERBOSE="${HAL_CACHE_CONTROL_PLANE_VERBOSE:-1}" \
  HAL_CACHE_HURL_VERBOSE="${HAL_CACHE_HURL_VERBOSE:-1}" \
  HAL_CACHE_CONTRACT_LOG_FILE="${TESTS_DIR}/contracts/hurl/build-${CONTROL_PLANE_PORT}-$(date +%s).log" \
  bash "${TESTS_DIR}/run-tests.sh" --contract-only

echo "Contract test output captured by hurl in test process; check console for pass/fail details above"
