#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
NODE_DIR="$(cd -- "$SCRIPT_DIR/.." && pwd)"

cd "$NODE_DIR"

echo "Starting cache-node"
./bin/cache-node "$@"
