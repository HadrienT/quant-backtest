#ifndef QUANTBACKTEST_INFERENCE_SHARPE_INFERENCE_HPP
#define QUANTBACKTEST_INFERENCE_SHARPE_INFERENCE_HPP

#include "quantBacktest/core/estimate.hpp"
#include "quantBacktest/core/types.hpp"

#include <span>

namespace quantBacktest
{

    // A Sharpe ratio is an estimate (companion, section 8). In this file the
    // Sharpe ratios are **per period**, not annualised, unless said otherwise;
    // `skewness` and `kurtosis` are those of metrics/performance.hpp (0 and 3
    // for a Gaussian).

    /// Standard error of the Sharpe ratio of T independent returns: Lo, "The
    /// Statistics of Sharpe Ratios", Financial Analysts Journal 58(4), 2002,
    /// with the correction for non-normal returns of Mertens, "Comments on
    /// Variance of the IID Estimator in Lo (2002)", 2002:
    ///
    ///     SE = sqrt((1 - skewness SR + (kurtosis - 1) / 4 SR^2) / T)
    ///
    /// which is sqrt((1 + SR^2 / 2) / T) for Gaussian returns. It is not valid
    /// for autocorrelated returns: use the bootstrap then.
    Real sharpe_standard_error(Real sharpe, Real skewness, Real kurtosis, Index observations);

    /// The **annualised** Sharpe ratio of a series of excess returns, with the
    /// standard error above and a normal confidence interval.
    Estimate sharpe_estimate(std::span<const Real> excess, Real periods_per_year, Real confidence);

    /// Probabilistic Sharpe Ratio: the probability that the true Sharpe ratio
    /// exceeds `threshold`. Bailey and Lopez de Prado, "The Sharpe Ratio
    /// Efficient Frontier", Journal of Risk 15(2), 2012:
    ///
    ///     PSR = Phi((SR - threshold) sqrt(T - 1) / sqrt(1 - skewness SR + (kurtosis - 1) / 4 SR^2))
    Real probabilistic_sharpe_ratio(Real sharpe, Real threshold, Real skewness, Real kurtosis, Index observations);

    /// Minimum track record length, same reference: the number of
    /// observations needed for PSR(threshold) to reach `confidence`.
    ///
    ///     MinTRL = 1 + (1 - skewness SR + (kurtosis - 1) / 4 SR^2) (z / (SR - threshold))^2
    ///
    /// Infinite when the Sharpe ratio does not exceed the threshold.
    Real minimum_track_record_length(Real sharpe, Real threshold, Real skewness, Real kurtosis, Real confidence);

} // namespace quantBacktest

#endif
