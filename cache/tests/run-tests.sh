#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
CONTRACT_SUITE_DIR="${SCRIPT_DIR}/contracts/hurl"
CONTRACT_SUITE_FILE="${CONTRACT_SUITE_DIR}/cache-control-plane-full-lifecycle.hurl"

BASE_URL="${HAL_CACHE_CONTROL_PLANE_URL:-http://localhost:8080}"
RUN_ID="${HAL_CACHE_CONTRACT_RUN_ID:-$(date +%s)}"
HURL_BIN="${HURL_BIN:-hurl}"
CONTRACT_ONLY=false

usage() {
  cat <<'USAGE'
Usage: cache/tests/run-tests.sh [options]

Options:
  --contract-only       Run only contract tests (Hurl suite)
  --base-url URL        Control-plane base URL (default: HAL_CACHE_CONTROL_PLANE_URL or http://localhost:8080)
  --run-id ID           Unique run suffix for instance IDs and keys (default: HAL_CACHE_CONTRACT_RUN_ID or epoch seconds)
  --hurl-bin PATH       Hurl binary to use (default: HURL_BIN or hurl)
  -h, --help            Show this help
USAGE
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --contract-only)
      CONTRACT_ONLY=true
      shift
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
  "${HURL_BIN}" \
    --test \
    --location \
    --variable "base_url=${BASE_URL}" \
    --variable "run_id=${RUN_ID}" \
    "${CONTRACT_SUITE_FILE}"
}

if [[ "${CONTRACT_ONLY}" == "true" ]]; then
  run_contract_suite
  exit 0
fi

run_contract_suite
