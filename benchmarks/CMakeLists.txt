find_package(benchmark CONFIG REQUIRED)

add_executable(quantBacktest_bench
  bench_engine.cpp
)
target_link_libraries(quantBacktest_bench
  PRIVATE
    quantBacktest
    benchmark::benchmark_main
)
# The synthetic market of the tests.
target_include_directories(quantBacktest_bench PRIVATE ${PROJECT_SOURCE_DIR}/tests)
qb_set_warnings(quantBacktest_bench)
