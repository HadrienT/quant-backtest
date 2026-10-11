#ifndef QUANTBACKTEST_DATA_RETURNS_HPP
#define QUANTBACKTEST_DATA_RETURNS_HPP

#include "quantBacktest/core/types.hpp"
#include "quantBacktest/data/market_data.hpp"

#include <span>
#include <vector>

namespace quantBacktest
{

    /// Arithmetic total returns from raw closes and corporate actions
    /// (companion, sections 0 and 3.3), one row per date, row-major:
    ///
    ///     R[t, i] = s (P[t, i] + D[t, i]) / P[t-1, i] - 1
    ///
    /// where s is the split ratio and D the dividend per share taking effect
    /// on day t. The first row is NaN, and so is any return whose close or
    /// previous close is missing: a return is never taken across a gap.
    std::vector<Real> total_returns(const MarketData &market);

    /// Simple rates per period from annualized simple rates:
    ///
    ///     out[t] = annual[t-1] (dates[t] - dates[t-1]) / basis,   out[0] = 0
    ///
    /// The rate applied over a period is the one known when the period starts.
    /// `basis` is the number of days of the money-market year (360 for USD).
    std::vector<Real> period_rates_from_annualized(std::span<const Date> dates, std::span<const Real> annual, Real basis);

} // namespace quantBacktest

#endif
