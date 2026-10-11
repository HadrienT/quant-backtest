#ifndef QUANTBACKTEST_INFERENCE_SHARPE_DIFFERENCE_HPP
#define QUANTBACKTEST_INFERENCE_SHARPE_DIFFERENCE_HPP

#include "quantBacktest/core/estimate.hpp"
#include "quantBacktest/core/run_control.hpp"
#include "quantBacktest/core/types.hpp"
#include "quantBacktest/inference/bootstrap.hpp"

#include <optional>
#include <span>

namespace quantBacktest
{

    /// Two Sharpe ratios measured on the same period are correlated: their
    /// intervals are not compared one by one, their difference is tested
    /// (companion, section 8.4). It is the test to run between an optimised
    /// portfolio and the equal-weight one.
    struct SharpeDifference
    {
        /// Sharpe ratio of the first series minus the second, annualised,
        /// with the standard error of Jobson and Korkie, "Performance
        /// Hypothesis Testing with the Sharpe and Treynor Measures", Journal
        /// of Finance 36(4), 1981, as corrected by Memmel, "Performance
        /// Hypothesis Testing with the Sharpe Ratio", Finance Letters 1(1),
        /// 2003. With rho the correlation of the two series, per period:
        ///
        ///     Var = (2 - 2 rho + (SR1^2 + SR2^2 - 2 SR1 SR2 rho^2) / 2) / T
        ///
        /// Valid for independent Gaussian returns.
        Estimate analytical;
        /// difference over its standard error, and the two-sided probability
        /// of a larger one when the two Sharpe ratios are equal.
        Real z_statistic = 0.0;
        Real p_value = 0.0;
        Real correlation = 0.0;

        /// The same difference with a percentile interval from the stationary
        /// bootstrap of the two series resampled together, which keeps fat
        /// tails and autocorrelation. It follows the idea of Ledoit and Wolf,
        /// "Robust Performance Hypothesis Testing with the Sharpe Ratio",
        /// Journal of Empirical Finance 15(5), 2008, but not their studentised
        /// interval. Absent when the bootstrap is not run.
        std::optional<Estimate> bootstrap;
        /// Two-sided: twice the smaller share of the resampled differences on
        /// one side of zero.
        std::optional<Real> bootstrap_p_value;
    };

    /// The two series are excess returns over the same periods.
    /// `bootstrap.replications` at zero skips the bootstrap.
    SharpeDifference sharpe_difference(std::span<const Real> first, std::span<const Real> second, Real periods_per_year, Real confidence,
                                       const BootstrapConfig &bootstrap, const RunControl &control = {});

} // namespace quantBacktest

#endif
