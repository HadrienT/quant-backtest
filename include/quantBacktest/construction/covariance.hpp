#ifndef QUANTBACKTEST_CONSTRUCTION_COVARIANCE_HPP
#define QUANTBACKTEST_CONSTRUCTION_COVARIANCE_HPP

#include "quantBacktest/core/types.hpp"

#include <vector>

namespace quantBacktest
{

    /// A covariance matrix of N assets, row-major.
    struct Covariance
    {
        Index assets = 0;
        std::vector<Real> matrix;
        /// Weight given to the shrinkage target, in [0, 1]; 0 for the sample
        /// covariance.
        Real shrinkage = 0.0;

        MatrixView view() const noexcept { return {matrix.data(), assets, assets}; }
    };

    /// Mean of each column of `returns` (one row per period, one column per
    /// asset).
    std::vector<Real> mean_returns(MatrixView returns);

    /// Sample covariance, with the T - 1 denominator. It is singular as soon
    /// as there are not more periods than assets.
    Covariance sample_covariance(MatrixView returns);

    /// Ledoit and Wolf, "A Well-Conditioned Estimator for Large-Dimensional
    /// Covariance Matrices", Journal of Multivariate Analysis 88(2), 2004:
    /// the sample covariance S (T denominator, as in the paper) pulled towards
    /// the identity scaled by the mean variance m, with the intensity that
    /// minimises the expected squared error, estimated in closed form.
    ///
    ///     d^2 = ||S - m I||^2,   b^2 = min(d^2, (1/T^2) sum_t ||x_t x_t' - S||^2)
    ///     Sigma = (b^2 / d^2) m I + (1 - b^2 / d^2) S
    ///
    /// with ||A||^2 = trace(A A') / N. It is positive definite even with fewer
    /// periods than assets.
    Covariance ledoit_wolf_covariance(MatrixView returns);

} // namespace quantBacktest

#endif
