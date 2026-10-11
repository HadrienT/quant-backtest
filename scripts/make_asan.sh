#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

# Lower default than make.sh: ASan/UBSan roughly triples a process's memory
# footprint. Override: `NPROC=16 scripts/make_asan.sh`.
NPROC="${NPROC:-8}"

cmake --preset asan
cmake --build build-asan
GTEST_COLOR=1 ctest --test-dir build-asan --output-on-failure -j "$NPROC"
