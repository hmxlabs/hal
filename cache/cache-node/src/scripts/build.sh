#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
NODE_DIR="$(cd -- "$SCRIPT_DIR/.." && pwd)"

cd "$NODE_DIR"

echo "Building cache-node"
make all

echo "Running unit tests"
make test

echo "Running quality checks"
make check

echo "cache-node build/test/check completed"
