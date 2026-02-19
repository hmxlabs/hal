#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TEST_ENTRY="${SCRIPT_DIR}/all_tests.rb"

usage() {
  cat <<'EOF'
Usage: run-tests.sh [options]

Runs HAL cache test suites implemented in Ruby minitest.

Options:
  --base-url URL           Set CACHE_API_BASE_URL (enables API-backed tests)
  --insecure               Set CACHE_API_INSECURE=1
  --with-perf              Set CACHE_ENABLE_PERF=1
  --with-resilience CMD    Set CACHE_API_RESTART_CMD to CMD
  --recovery-timeout SEC   Set CACHE_RECOVERY_TIMEOUT
  --verbose                Pass --verbose to minitest
  --seed N                 Pass --seed N to minitest
  -h, --help               Show this help message

Examples:
  ./cache/tests/run-tests.sh
  ./cache/tests/run-tests.sh --base-url http://localhost:8080
  ./cache/tests/run-tests.sh --base-url http://localhost:8080 --with-perf
  ./cache/tests/run-tests.sh --base-url http://localhost:8080 --with-resilience "docker compose restart cache-control-plane"
EOF
}

if ! command -v ruby >/dev/null 2>&1; then
  echo "error: ruby is required but was not found in PATH" >&2
  exit 1
fi

RUBY_ARGS=()

while [[ $# -gt 0 ]]; do
  case "$1" in
    --base-url)
      [[ $# -ge 2 ]] || { echo "error: --base-url requires a value" >&2; exit 1; }
      export CACHE_API_BASE_URL="$2"
      shift 2
      ;;
    --insecure)
      export CACHE_API_INSECURE=1
      shift
      ;;
    --with-perf)
      export CACHE_ENABLE_PERF=1
      shift
      ;;
    --with-resilience)
      [[ $# -ge 2 ]] || { echo "error: --with-resilience requires a command string" >&2; exit 1; }
      export CACHE_API_RESTART_CMD="$2"
      shift 2
      ;;
    --recovery-timeout)
      [[ $# -ge 2 ]] || { echo "error: --recovery-timeout requires a value" >&2; exit 1; }
      export CACHE_RECOVERY_TIMEOUT="$2"
      shift 2
      ;;
    --verbose)
      RUBY_ARGS+=("--verbose")
      shift
      ;;
    --seed)
      [[ $# -ge 2 ]] || { echo "error: --seed requires a value" >&2; exit 1; }
      RUBY_ARGS+=("--seed" "$2")
      shift 2
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

echo "Running cache tests with:"
echo "  test entry: ${TEST_ENTRY}"
echo "  CACHE_API_BASE_URL: ${CACHE_API_BASE_URL:-<not set>}"
echo "  CACHE_ENABLE_PERF: ${CACHE_ENABLE_PERF:-0}"
echo "  CACHE_API_RESTART_CMD: ${CACHE_API_RESTART_CMD:-<not set>}"

if [[ ${#RUBY_ARGS[@]} -gt 0 ]]; then
  exec ruby "${TEST_ENTRY}" "${RUBY_ARGS[@]}"
else
  exec ruby "${TEST_ENTRY}"
fi
