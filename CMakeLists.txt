cmake_minimum_required(VERSION 3.25)

# The version is read from this line by pyproject.toml: keep it on one line.
project(quantBacktest VERSION 0.1.0 LANGUAGES CXX)

# ---- Build options ----
option(QB_BUILD_TESTS "Build unit tests (GoogleTest)" OFF)
option(QB_BUILD_PYTHON "Build Python bindings (pybind11)" OFF)
option(QB_BUILD_BENCHMARKS "Build benchmarks (google-benchmark)" OFF)
option(QB_BUILD_HEDGING "Build the hedging module, the only one that uses quantModeling" ON)

# ---- Global options ----
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/cmake)
include(BuildInfo)

function(qb_set_warnings target)
  target_compile_options(${target} PRIVATE
    $<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang>:-Wall -Wextra -Wpedantic -Wconversion -Wshadow>
  )
endfunction()

# ---- quantModeling, pinned (blueprint/decisions.md, ADR-003) ----
# Declared before the build stamp, which records the pinned commit.
if(QB_BUILD_HEDGING)
  include(QuantModeling)
else()
  set(QB_QUANTMODELING_SHA "")
endif()

# ---- Main library ----
# Everything except hedging/. It does not link quantModeling, so none of its
# sources can include a quantModeling header.
configure_file(
  ${PROJECT_SOURCE_DIR}/src/core/build_info.cpp.in
  ${PROJECT_BINARY_DIR}/generated/build_info.cpp
  @ONLY
)

add_library(quantBacktest
  ${PROJECT_BINARY_DIR}/generated/build_info.cpp
  src/core/types.cpp
  src/core/estimate.cpp
  src/core/fingerprint.cpp
  src/data/market_data.cpp
  src/data/returns.cpp
  src/quality/quality_report.cpp
  src/costs/spread_estimators.cpp
  src/costs/cost_model.cpp
  src/engine/backtest.cpp
  src/engine/vectorized.cpp
  src/metrics/performance.cpp
  src/inference/sharpe_inference.cpp
  src/inference/bootstrap.cpp
  src/inference/performance_report.cpp
  src/attribution/factor_regression.cpp
  src/risk_models/var_backtest.cpp
)

set_target_properties(quantBacktest PROPERTIES
  POSITION_INDEPENDENT_CODE ON
)

target_include_directories(quantBacktest
  PUBLIC
    ${PROJECT_SOURCE_DIR}/include
)

# Eigen stays out of the public headers: the interface is spans and vectors.
find_package(Eigen3 CONFIG REQUIRED)
find_package(Threads REQUIRED)
target_link_libraries(quantBacktest
  PRIVATE
    Eigen3::Eigen
  PUBLIC
    Threads::Threads
)

qb_set_warnings(quantBacktest)

# ---- Hedging module ----
# Header-less for now: lot B6 turns it into a compiled library. It exists from
# lot B0 so that the dependency on quantModeling has exactly one entry point.
if(QB_BUILD_HEDGING)
  add_library(quantBacktest_hedging INTERFACE)
  target_link_libraries(quantBacktest_hedging INTERFACE quantBacktest quantModeling)
endif()

# ---- Tests (GoogleTest) ----
if(QB_BUILD_TESTS)
  include(CTest)
  enable_testing()
  add_subdirectory(tests)
endif()

# ---- Benchmarks (google-benchmark) ----
if(QB_BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()

# ---- Python binding (pybind11) ----
if(QB_BUILD_PYTHON)
  add_subdirectory(bindings/python)
endif()
