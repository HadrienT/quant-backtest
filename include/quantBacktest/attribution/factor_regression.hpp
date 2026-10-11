#ifndef QUANTBACKTEST_ATTRIBUTION_FACTOR_REGRESSION_HPP
#define QUANTBACKTEST_ATTRIBUTION_FACTOR_REGRESSION_HPP

#include "quantBacktest/core/estimate.hpp"
#include "quantBacktest/core/types.hpp"

#include <span>
#include <vector>

namespace quantBacktest
{

    struct FactorRegressionConfig
    {
        /// 252 for daily returns: the alpha is the intercept times this.
        Real periods_per_year = 252.0;
        Real confidence = 0.95;
        /// Number of lags of the Newey-West covariance. Zero gives White's
        /// heteroskedasticity-consistent covariance. There is no default
        /// rule: the choice is the caller's, and it is written in the result.
        Index newey_west_lags = 0;
    };

    /// The regression of the companion, section 10.1:
    ///
    ///     Re[t] = alpha + sum_k beta[k] F[t, k] + e[t]
    ///
    /// by ordinary least squares, with the covariance of Newey and West, "A
    /// Simple, Positive Semi-Definite, Heteroskedasticity and Autocorrelation
    /// Consistent Covariance Matrix", Econometrica 55(3), 1987:
    ///
    ///     V = (X'X)^-1 S (X'X)^-1
    ///     S = G(0) + sum_{l=1..L} (1 - l / (L + 1)) (G(l) + G(l)')
    ///     G(l) = sum_t e[t] e[t-l] x[t] x[t-l]'
    ///
    /// without small-sample correction. Intervals use the normal distribution.
    struct FactorRegression
    {
        /// Annualised: the intercept times the number of periods per year,
        /// and its standard error likewise (finding 4).
        Estimate alpha;
        /// One per factor, in the order of the columns.
        std::vector<Estimate> betas;
        /// alpha over its standard error, and the same for each beta.
        Real alpha_t_statistic = 0.0;
        std::vector<Real> beta_t_statistics;
        Real r_squared = 0.0;
        /// Standard deviation of the residuals, annualised.
        Real residual_volatility = 0.0;
        Index observations = 0;
        Index newey_west_lags = 0;
    };

    /// `excess` holds the excess returns of the portfolio, `factors` one row
    /// per period and one column per factor (market excess return alone for
    /// the CAPM; Fama-French, momentum). Factor returns are excess or
    /// long-short returns already.
    FactorRegression factor_regression(std::span<const Real> excess, MatrixView factors, const FactorRegressionConfig &config);

    /// The betas of the same regression on every window of `window` periods:
    /// one row per period and one column per factor, row-major. Row t uses
    /// the periods t - window + 1 to t; the first window - 1 rows are NaN.
    std::vector<Real> rolling_betas(std::span<const Real> excess, MatrixView factors, Index window);

} // namespace quantBacktest

#endif
