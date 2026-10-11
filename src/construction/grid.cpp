#include "quantBacktest/construction/grid.hpp"

#include "quantBacktest/core/parallel.hpp"

#include <string>

namespace quantBacktest
{

    GridResult run_allocation_grid(const MarketData &market, std::span<const AllocationConfig> configurations, const BacktestConfig &backtest,
                                   Index threads, const RunControl &control)
    {
        if (configurations.empty())
        {
            throw Error("the grid needs at least one configuration");
        }
        // The data are controlled once, here, and not again by each run.
        const QualityReport quality = backtest.data_already_checked ? check_market_structure(market) : check_market_data(market, backtest.quality);
        if (!quality.usable())
        {
            throw Error("the data cannot be used:\n" + quality.error_summary());
        }
        BacktestConfig each = backtest;
        each.data_already_checked = true;

        const Index count = configurations.size();
        std::vector<BacktestResult> results(count);
        GridResult out;
        out.configurations = count;
        out.descriptions.resize(count);
        parallel_for(count, threads, control, [&](Index m)
                     {
                         try
                         {
                             AllocationStrategy strategy(configurations[m]);
                             out.descriptions[m] = strategy.describe();
                             results[m] = run_backtest(market, strategy, each);
                         }
                         catch (const Cancelled &)
                         {
                             throw;
                         }
                         catch (const Error &error)
                         {
                             throw Error("configuration " + std::to_string(m) + " of the grid: " + error.what());
                         } });

        const std::vector<Date> &dates = results.front().dates;
        if (dates.size() < 2)
        {
            throw Error("the grid needs at least two days of book to compute a return");
        }
        const Index rows = dates.size() - 1;
        out.dates.assign(dates.begin() + 1, dates.end());
        out.returns.resize(rows * count);
        out.final_values.resize(count);
        for (Index m = 0; m < count; ++m)
        {
            const std::vector<Real> &value = results[m].value;
            for (Index t = 0; t < rows; ++t)
            {
                out.returns[t * count + m] = value[t + 1] / value[t] - 1.0;
            }
            out.final_values[m] = value.back();
        }
        return out;
    }

} // namespace quantBacktest
