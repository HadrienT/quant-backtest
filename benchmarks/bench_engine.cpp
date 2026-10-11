// One run of the accounting engine at the size the site will use: 500 assets
// over 26 years of daily bars (blueprint/decisions.md, ADR-002).
#include <benchmark/benchmark.h>

#include "quantBacktest/costs/spread_estimators.hpp"
#include "quantBacktest/data/returns.hpp"
#include "quantBacktest/engine/backtest.hpp"
#include "quantBacktest/engine/vectorized.hpp"
#include "quantBacktest/quality/quality_report.hpp"

#include "support/synthetic_market.hpp"

#include <vector>

namespace
{

    using namespace quantBacktest;

    constexpr Index Days = 6552;
    constexpr Index Assets = 500;

    const testing::SyntheticMarket &market()
    {
        static const testing::SyntheticMarket m = testing::makeMarket(Days, Assets, 2026);
        return m;
    }

    std::vector<Real> equalWeights()
    {
        return std::vector<Real>(Assets, 1.0 / static_cast<Real>(Assets));
    }

    void runEqualWeight(benchmark::State &state, Index period, const CostModel *costs)
    {
        const MarketData data = market().data();
        BacktestConfig config;
        config.initial_capital = 1e8;
        config.costs = costs;
        // The controls are measured on their own, below: a validation scheme
        // runs them once and the book thousands of times.
        config.data_already_checked = true;
        for (auto _ : state)
        {
            testing::ConstantWeights strategy(equalWeights(), period);
            benchmark::DoNotOptimize(run_backtest(data, strategy, config));
        }
    }

    void EqualWeightMonthly(benchmark::State &state)
    {
        const ProportionalCost cost(0.0005);
        runEqualWeight(state, 21, &cost);
    }

    void EqualWeightDaily(benchmark::State &state)
    {
        const ProportionalCost cost(0.0005);
        runEqualWeight(state, 1, &cost);
    }

    void EqualWeightMonthlyEstimatedSpread(benchmark::State &state)
    {
        const std::vector<Real> spreads = estimated_spreads(market().data(), SpreadEstimator::CorwinSchultz, 21, 10);
        const SpreadCost cost({spreads.data(), Days, Assets}, "Corwin and Schultz, 21 days");
        runEqualWeight(state, 21, &cost);
    }

    // The table of spreads, computed once for any number of runs.
    void EstimatedSpreads(benchmark::State &state)
    {
        const MarketData data = market().data();
        for (auto _ : state)
        {
            benchmark::DoNotOptimize(estimated_spreads(data, SpreadEstimator::CorwinSchultz, 21, 10));
        }
    }

    // The part of a run that is not the book: the quality controls.
    void QualityControls(benchmark::State &state)
    {
        const MarketData data = market().data();
        for (auto _ : state)
        {
            benchmark::DoNotOptimize(check_market_data(data));
        }
    }

    // The vectorized backtest of the same daily rebalancing, without costs.
    void VectorizedDaily(benchmark::State &state)
    {
        const MarketData data = market().data();
        const std::vector<Real> returns = total_returns(data);
        std::vector<Real> weights(Days * Assets, 1.0 / static_cast<Real>(Assets));
        for (auto _ : state)
        {
            benchmark::DoNotOptimize(vectorized_values({returns.data(), Days, Assets}, {weights.data(), Days, Assets}, data.cash_rate, 1e8));
        }
    }

} // namespace

BENCHMARK(EqualWeightMonthly)->Unit(benchmark::kMillisecond);
BENCHMARK(EqualWeightDaily)->Unit(benchmark::kMillisecond);
BENCHMARK(EqualWeightMonthlyEstimatedSpread)->Unit(benchmark::kMillisecond);
BENCHMARK(EstimatedSpreads)->Unit(benchmark::kMillisecond);
BENCHMARK(QualityControls)->Unit(benchmark::kMillisecond);
BENCHMARK(VectorizedDaily)->Unit(benchmark::kMillisecond);
