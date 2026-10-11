#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

# ctest runs each TEST() as its own process (gtest_discover_tests), so -j
# parallelizes across independent processes. Override: `NPROC=8 scripts/make.sh`.
NPROC="${NPROC:-16}"

cmake --preset default
cmake --build build
GTEST_COLOR=1 ctest --test-dir build --output-on-failure -j "$NPROC"
