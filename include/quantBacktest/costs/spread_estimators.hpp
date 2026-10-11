#ifndef QUANTBACKTEST_COSTS_SPREAD_ESTIMATORS_HPP
#define QUANTBACKTEST_COSTS_SPREAD_ESTIMATORS_HPP

#include "quantBacktest/core/types.hpp"
#include "quantBacktest/data/market_data.hpp"

#include <span>
#include <vector>

namespace quantBacktest
{

    // Bid-ask spread estimated from daily bars, without an order book
    // (companion, section 5.1). The two functions below take consecutive days
    // of one asset, in one share count, and return the proportional spread:
    // ask minus bid, over the mid price. `estimated_spreads` applies one of
    // them to a whole market.
    //
    // Both are built for wide spreads. When the spread is narrow next to the
    // daily volatility the estimate is noise, of the order of 0.4 (Corwin and
    // Schultz) or 0.2 (Abdi and Ranaldo) times that volatility on a monthly
    // window: on a liquid large capitalisation it overstates the spread by an
    // order of magnitude (tests/testCosts.cpp measures it). Read the result as
    // an upper bound there, not as a cost.
    //
    // A pair of consecutive days with a missing value is skipped. The result
    // is NaN when fewer than `min_pairs` pairs remain: an estimate that cannot
    // be made is not replaced by anything.

    /// Corwin and Schultz, "A Simple Way to Estimate Bid-Ask Spreads from
    /// Daily High and Low Prices", Journal of Finance 67(2), 2012.
    ///
    /// For each pair of days (t, t+1), with the overnight adjustment of their
    /// section 2.1 (if the close of t is outside the range of t+1, the high
    /// and low of t+1 are shifted by the gap):
    ///
    ///     beta  = ln(H_t / L_t)^2 + ln(H_t+1 / L_t+1)^2
    ///     gamma = ln(H_t,t+1 / L_t,t+1)^2
    ///     alpha = (sqrt(2 beta) - sqrt(beta)) / (3 - 2 sqrt(2)) - sqrt(gamma / (3 - 2 sqrt(2)))
    ///     S     = 2 (e^alpha - 1) / (1 + e^alpha)
    ///
    /// Negative two-day estimates are set to zero and the estimates are
    /// averaged (their section 2.3). Their treatment of days where the high
    /// equals the low (section 2.2) is not applied: such a day is used as it
    /// is.
    Real corwin_schultz_spread(std::span<const Real> high, std::span<const Real> low, std::span<const Real> close, Index min_pairs = 1);

    /// Abdi and Ranaldo, "A Simple Estimation of Bid-Ask Spreads from Daily
    /// Close, High, and Low Prices", Review of Financial Studies 30(12), 2017,
    /// in its "monthly corrected" version (their equation 10). With c the log
    /// close and eta the mid-range, the mean of the log high and the log low:
    ///
    ///     S = sqrt(max(4 mean((c_t - eta_t)(c_t - eta_t+1)), 0))
    Real abdi_ranaldo_spread(std::span<const Real> high, std::span<const Real> low, std::span<const Real> close, Index min_pairs = 1);

    enum class SpreadEstimator
    {
        CorwinSchultz,
        AbdiRanaldo,
    };

    /// The spread of every asset on every day, one row per date, row-major:
    /// row t is the estimate over the `window` days that precede t, day t
    /// excluded, so that it was known before trading on t. Splits inside the
    /// window are undone. NaN where fewer than `min_pairs` two-day pairs are
    /// available, and on the first `window` days.
    ///
    /// Needs the 'high' and 'low' tables; throws `Error` otherwise.
    std::vector<Real> estimated_spreads(const MarketData &market, SpreadEstimator estimator, Index window, Index min_pairs);

} // namespace quantBacktest

#endif
