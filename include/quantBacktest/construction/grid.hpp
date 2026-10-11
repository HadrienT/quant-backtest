#ifndef QUANTBACKTEST_CONSTRUCTION_GRID_HPP
#define QUANTBACKTEST_CONSTRUCTION_GRID_HPP

#include "quantBacktest/construction/allocation_strategy.hpp"
#include "quantBacktest/core/run_control.hpp"
#include "quantBacktest/core/types.hpp"
#include "quantBacktest/engine/backtest.hpp"

#include <span>
#include <string>
#include <vector>

namespace quantBacktest
{

    /// The table the validation module works on (validation/walk_forward.hpp):
    /// one row per day, one column per configuration.
    struct GridResult
    {
        /// The day each row of returns ends on: from the second day of the
        /// book to the last.
        std::vector<Date> dates;
        Index configurations = 0;
        /// Net daily returns of each configuration, value[t] / value[t-1] - 1,
        /// row-major.
        std::vector<Real> returns;
        /// What each configuration is, in English, and its final value.
        std::vector<std::string> descriptions;
        std::vector<Real> final_values;

        MatrixView view() const noexcept { return {returns.data(), dates.size(), configurations}; }
    };

    /// Runs one backtest per configuration, on the same data, the same costs
    /// and the same execution rules, on `threads` threads. Each run is
    /// independent: the table does not depend on the number of threads.
    ///
    /// Every configuration tried belongs in the grid, the ones that will be
    /// discarded included: the number of columns is the number of trials the
    /// deflated Sharpe ratio corrects for. A configuration that fails (a cap
    /// too low, a singular covariance) fails the whole grid, with its index
    /// in the message.
    GridResult run_allocation_grid(const MarketData &market, std::span<const AllocationConfig> configurations, const BacktestConfig &backtest,
                                   Index threads = 1, const RunControl &control = {});

} // namespace quantBacktest

#endif
