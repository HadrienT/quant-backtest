#ifndef QUANTBACKTEST_METRICS_COST_REPORT_HPP
#define QUANTBACKTEST_METRICS_COST_REPORT_HPP

#include "quantBacktest/core/estimate.hpp"
#include "quantBacktest/core/types.hpp"
#include "quantBacktest/engine/backtest.hpp"

#include <optional>
#include <span>

namespace quantBacktest
{

    /// What a strategy's trading costs it (companion, section 5.2).
    struct CostSummary
    {
        /// Mean daily turnover times the number of periods per year: one-way,
        /// half the amount traded over the portfolio value. A turnover of 10
        /// a year with 10 basis points per trade costs one point a year.
        Real annualized_turnover = 0.0;
        /// Costs paid over the portfolio value, per year, in basis points.
        Real annual_cost_bps = 0.0;
        /// The proportional cost per trade that brings the excess return to
        /// zero: the annualised mean excess return of the run **without
        /// costs** over the amount traded per year (twice the turnover). Its
        /// uncertainty is the one of the mean return. Absent when the run did
        /// not trade.
        ///
        /// A break-even of 8 basis points with a realistic cost of 5 means
        /// the strategy hangs by a thread.
        std::optional<Estimate> break_even_cost;
    };

    /// `net` is the run with its costs, `gross` the same run without any
    /// (the net run itself if it had none). `cash_rate` is the series given to
    /// the engine, one rate per date of the market; `start_index` the first
    /// day of the book.
    CostSummary summarize_costs(const BacktestResult &net, const BacktestResult &gross, std::span<const Real> cash_rate, Index start_index,
                                Real periods_per_year, Real confidence);

} // namespace quantBacktest

#endif
