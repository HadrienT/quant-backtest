#ifndef QUANTBACKTEST_METRICS_PERFORMANCE_HPP
#define QUANTBACKTEST_METRICS_PERFORMANCE_HPP

#include "quantBacktest/core/types.hpp"

#include <span>
#include <vector>

namespace quantBacktest
{

    // The metrics of the companion, section 7.1: one function per metric, its
    // convention in its comment (section 7.2). References: Bacon, "Practical
    // Portfolio Performance Measurement and Attribution", 2nd ed., 2008.
    //
    // Returns are arithmetic, per period. `periods_per_year` is 252 for daily
    // data. Every function throws `Error` on a series too short for it or
    // holding a missing value: nothing is skipped silently.

    /// R[t] = V[t] / V[t-1] - 1, one value fewer than the values.
    std::vector<Real> simple_returns(std::span<const Real> values);

    /// The value path of 1 unit invested: V[0] = 1, V[t] = V[t-1] (1 + R[t]),
    /// one value more than the returns.
    std::vector<Real> cumulative_values(std::span<const Real> returns);

    /// Excess returns, period by period: R[t] - rf[t], with the rate of each
    /// period, never one rate for the whole history (finding 3). `cash_rate`
    /// holds the simple rate of each period, aligned with `returns`.
    std::vector<Real> excess_returns(std::span<const Real> returns, std::span<const Real> cash_rate);

    /// Sample mean, and standard deviation with the n-1 denominator.
    Real mean(std::span<const Real> x);
    Real standard_deviation(std::span<const Real> x);

    /// Skewness m3 / m2^1.5 and kurtosis m4 / m2^2, with moments about the
    /// mean and the n denominator: 0 and 3 for a Gaussian. NaN if the series
    /// is constant.
    Real skewness(std::span<const Real> x);
    Real kurtosis(std::span<const Real> x);

    /// Geometric annualised return: (prod (1 + R[t]))^(periods_per_year / T) - 1.
    Real cagr(std::span<const Real> returns, Real periods_per_year);

    /// Standard deviation of the returns times sqrt(periods_per_year). Exact
    /// only for returns that are not autocorrelated (Lo 2002).
    Real annualized_volatility(std::span<const Real> returns, Real periods_per_year);

    /// Sharpe (1966, 1994): mean over standard deviation of the excess
    /// returns, per period. Arithmetic returns, not logarithmic (finding 10).
    ///
    /// A series whose volatility is zero (below 1e-12 per period) has a
    /// Sharpe ratio of zero when its mean is zero too, as cash alone does,
    /// and no Sharpe ratio (NaN) otherwise.
    Real sharpe_ratio(std::span<const Real> excess);

    /// Sortino and Price (1994): mean excess return over the downside
    /// deviation sqrt(mean(min(R, 0)^2)), per period. NaN without any
    /// negative return.
    Real sortino_ratio(std::span<const Real> excess);

    /// Drawdown of each period, V[t] / max(V[s], s <= t) - 1: zero or negative.
    std::vector<Real> drawdowns(std::span<const Real> values);

    /// The deepest drawdown: zero or negative.
    Real max_drawdown(std::span<const Real> values);

    /// Longest stretch below a previous high, in periods.
    Index longest_underwater(std::span<const Real> values);

    /// CAGR over the absolute maximum drawdown. NaN without any drawdown.
    Real calmar_ratio(std::span<const Real> returns, Real periods_per_year);

    /// Historical value-at-risk and expected shortfall at tail probability
    /// `tail` (0.01 or 0.05), both as **positive losses**: with k = ceil(tail
    /// T) and the returns sorted in increasing order, VaR = -R(k) and ES is
    /// minus the mean of the k lowest returns.
    Real historical_var(std::span<const Real> returns, Real tail);
    Real historical_expected_shortfall(std::span<const Real> returns, Real tail);

    /// Standard deviation of R - R_benchmark, times sqrt(periods_per_year).
    Real tracking_error(std::span<const Real> returns, std::span<const Real> benchmark, Real periods_per_year);

    /// Mean over standard deviation of R - R_benchmark, per period: the
    /// Sharpe ratio of the active bet.
    Real information_ratio(std::span<const Real> returns, std::span<const Real> benchmark);

    /// Fraction of the periods with a positive return, and mean gain over
    /// mean loss (NaN without a gain or without a loss). To be read together.
    Real win_rate(std::span<const Real> returns);
    Real gain_loss_ratio(std::span<const Real> returns);

    /// Return of each calendar year present in `dates`, compounded from the
    /// returns of that year. `dates[t]` is the date return t ends on.
    struct YearReturn
    {
        int year = 0;
        Real value = 0.0;
        Index observations = 0;
    };
    std::vector<YearReturn> calendar_year_returns(std::span<const Date> dates, std::span<const Real> returns);

} // namespace quantBacktest

#endif
