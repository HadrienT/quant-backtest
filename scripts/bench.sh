#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

cmake --preset bench
cmake --build build-bench --target quantBacktest_bench
build-bench/benchmarks/quantBacktest_bench "$@"
