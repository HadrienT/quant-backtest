#!/usr/bin/env bash
#
# Builds the `quantbacktest` wheel into dist/.
#
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

# Eigen3, which quantModeling links against, is only findable through the vcpkg
# toolchain file. pyproject.toml cannot reference $VCPKG_ROOT (TOML has no env
# interpolation), so it goes through CMAKE_ARGS, which scikit-build-core
# forwards to the CMake configure step.
if [ -z "${VCPKG_ROOT:-}" ]; then
  echo "VCPKG_ROOT is not set: the wheel needs the vcpkg toolchain." >&2
  exit 1
fi
export CMAKE_ARGS="${CMAKE_ARGS:-} -DCMAKE_TOOLCHAIN_FILE=${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"

# uv if present (a uv-created environment has no pip), otherwise pip + build.
if command -v uv >/dev/null 2>&1; then
  uv build --wheel --out-dir dist
else
  python -m pip install --upgrade pip build
  python -m build --wheel
fi

echo "Wheel built in dist/"
