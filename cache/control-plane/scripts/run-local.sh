#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
BIN="${ROOT_DIR}/bin/cache-control-plane"
REDIS_PORT="${HAL_CACHE_REDIS_PORT:-6379}"
HTTP_PORT="${HAL_CACHE_CONTROL_PLANE_PORT:-8080}"

if ! command -v redis-server >/dev/null 2>&1; then
  echo "error: redis-server not found in PATH" >&2
  exit 127
fi

if ! command -v redis-cli >/dev/null 2>&1; then
  echo "error: redis-cli not found in PATH" >&2
  exit 127
fi

if ! [ -x "${BIN}" ]; then
  make -C "${ROOT_DIR}" all
fi

if ! redis-cli -p "${REDIS_PORT}" ping >/dev/null 2>&1; then
  echo "Starting Redis on port ${REDIS_PORT}"
  redis-server --port "${REDIS_PORT}" --save "" --appendonly no --daemonize yes
  sleep 1
fi

HAL_CACHE_CONTROL_PLANE_VERBOSE="${HAL_CACHE_CONTROL_PLANE_VERBOSE:-1}" \
  exec "${BIN}" --port "${HTTP_PORT}" --redis-port "${REDIS_PORT}"
