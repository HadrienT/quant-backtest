#ifndef QUANTBACKTEST_RISK_MODELS_VAR_BACKTEST_HPP
#define QUANTBACKTEST_RISK_MODELS_VAR_BACKTEST_HPP

#include "quantBacktest/core/types.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace quantBacktest
{

    // The backtest of a value-at-risk model (companion, section 12): losses
    // realised against losses announced. The engine computes neither the P&L
    // nor the VaR: both series come from the caller. The model is judged on
    // the hypothetical P&L, the one of yesterday's portfolio held still.

    /// I[t] = 1 when the loss of day t exceeds the VaR announced for that day:
    /// pnl[t] < -value_at_risk[t]. The VaR is a positive loss, the P&L is
    /// signed, and the two series are aligned on the day the P&L is realised.
    std::vector<std::uint8_t> var_exceptions(std::span<const Real> pnl, std::span<const Real> value_at_risk);

    /// A likelihood ratio and the probability, under the hypothesis tested,
    /// of a ratio at least as large (chi-square with that many degrees of
    /// freedom). Both are NaN when the test cannot be run on the sample.
    struct LikelihoodRatio
    {
        Real statistic = 0.0;
        Real p_value = 0.0;
        int degrees_of_freedom = 0;
    };

    struct CoverageTests
    {
        Index observations = 0;
        Index exceptions = 0;
        /// tail_probability times observations.
        Real expected_exceptions = 0.0;

        /// Kupiec, "Techniques for Verifying the Accuracy of Risk Measurement
        /// Models", Journal of Derivatives 3(2), 1995. Is the number x of
        /// exceptions over T days compatible with the probability p?
        ///
        ///     LR_uc = -2 ln[(1-p)^(T-x) p^x / ((1-x/T)^(T-x) (x/T)^x)]
        LikelihoodRatio unconditional_coverage;

        /// Christoffersen, "Evaluating Interval Forecasts", International
        /// Economic Review 39(4), 1998. Do exceptions come in clusters? With
        /// n_ij the number of days in state j that follow a day in state i,
        /// pi_01 = n_01 / (n_00 + n_01), pi_11 = n_11 / (n_10 + n_11) and
        /// pi = (n_01 + n_11) / (n_00 + n_01 + n_10 + n_11):
        ///
        ///     LR_ind = -2 ln[(1-pi)^(n_00+n_10) pi^(n_01+n_11)
        ///                    / ((1-pi_01)^n_00 pi_01^n_01 (1-pi_11)^n_10 pi_11^n_11)]
        ///
        /// Not defined when the sample has no exception, or nothing else.
        LikelihoodRatio independence;

        /// LR_cc = LR_uc + LR_ind, chi-square with 2 degrees of freedom.
        LikelihoodRatio conditional_coverage;

        /// transitions[i][j] = n_ij.
        Index transitions[2][2] = {{0, 0}, {0, 0}};
    };

    /// `tail_probability` is p: 0.01 for a 99 % VaR.
    ///
    /// The chi-square laws are large-sample approximations. With 250 days and
    /// p = 1 %, 2.5 exceptions are expected: measured on an exact model, at a
    /// nominal level of 5 %, the Kupiec test rejects 10 % of the time (the
    /// count is an integer, and no exception at all is already a rejection)
    /// and the independence test 1.7 %. Both reach 5 % on large samples.
    CoverageTests var_coverage_tests(std::span<const std::uint8_t> exceptions, Real tail_probability);

    enum class Zone
    {
        Green,
        Amber,
        Red,
    };

    /// The three zones of the Basel Committee, "Supervisory Framework for the
    /// Use of Backtesting in Conjunction with the Internal Models Approach to
    /// Market Risk Capital Requirements", 1996: amber starts at the number of
    /// exceptions that an exact model reaches or stays under with a
    /// probability of 95 % or more, red at 99.99 %.
    struct TrafficLight
    {
        Zone zone = Zone::Green;
        /// Probability that an exact model shows this many exceptions or fewer.
        Real cumulative_probability = 0.0;

        /// The supervisory figures exist for one sample only: 250 days at
        /// 99 %. Elsewhere they are absent.
        ///
        /// Increase of the multiplier of 3 in the 1996 framework: 0 in the
        /// green zone; 0.40, 0.50, 0.65, 0.75, 0.85 for 5 to 9 exceptions;
        /// 1.00 in the red zone.
        std::optional<Real> basel_plus_factor;
        /// Multiplier of the Basel Framework, MAR33 (FRTB): 1.50 in the green
        /// zone; 1.70, 1.76, 1.83, 1.88, 1.92 for 5 to 9 exceptions; 2.00 in
        /// the red zone.
        std::optional<Real> frtb_multiplier;
    };

    TrafficLight traffic_light(Index exceptions, Index observations, Real tail_probability);

    /// "green", "amber" or "red".
    std::string zone_name(Zone zone);

} // namespace quantBacktest

#endif
