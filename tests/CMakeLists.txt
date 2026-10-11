find_package(GTest CONFIG REQUIRED)
include(GoogleTest)

# One executable for everything that must build and pass without quantModeling,
# one for hedging/. A test that needs a pricer goes in the second.
add_executable(quantBacktest_tests
  testBuildInfo.cpp
  testCore.cpp
  testData.cpp
  testQuality.cpp
  testCosts.cpp
  testEngine.cpp
  testMetrics.cpp
  testInference.cpp
  testAttribution.cpp
  testConstruction.cpp
  testRiskModels.cpp
)
target_link_libraries(quantBacktest_tests
  PRIVATE
    quantBacktest
    GTest::gtest_main
)
qb_set_warnings(quantBacktest_tests)
gtest_discover_tests(quantBacktest_tests)

if(QB_BUILD_HEDGING)
  add_executable(quantBacktest_hedging_tests
    testHedgingPricing.cpp
    testHedging.cpp
  )
  # testHedgingPricing.cpp prices with quantModeling directly.
  target_link_libraries(quantBacktest_hedging_tests
    PRIVATE
      quantBacktest_hedging
      quantModeling
      GTest::gtest_main
  )
  qb_set_warnings(quantBacktest_hedging_tests)
  gtest_discover_tests(quantBacktest_hedging_tests PROPERTIES LABELS hedging)
endif()
