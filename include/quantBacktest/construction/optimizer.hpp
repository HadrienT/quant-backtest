#ifndef QUANTBACKTEST_CONSTRUCTION_OPTIMIZER_HPP
#define QUANTBACKTEST_CONSTRUCTION_OPTIMIZER_HPP

#include "quantBacktest/core/types.hpp"

#include <span>
#include <vector>

namespace quantBacktest
{

    // Portfolio construction (companion, section 6). Weights sum to one. The
    // bounds of each weight are constraints of the optimisation itself: they
    // are never applied afterwards by clipping and renormalising, which
    // breaks the other constraints (finding 5). A problem without a solution
    // is an `Error`, never a best effort.
    //
    // Every covariance must be positive definite; the sample covariance of
    // fewer periods than assets is not, and is refused.

    /// 1/N.
    std::vector<Real> equal_weights(Index assets);

    /// w' Sigma w.
    Real portfolio_variance(MatrixView covariance, std::span<const Real> weights);

    /// Share of the variance carried by each position, w_i (Sigma w)_i over
    /// w' Sigma w: they sum to one.
    std::vector<Real> risk_contributions(MatrixView covariance, std::span<const Real> weights);

    /// Minimises (1/2) w' Sigma w - linear' w subject to sum(w) = 1 and
    /// lower <= w <= upper, exactly: a weight at a bound is equal to it. With
    /// `linear` empty, this is the minimum-variance portfolio.
    ///
    /// Solved by a primal active-set method (blueprint/decisions.md, ADR-010).
    /// `start` may hold an admissible portfolio to start from.
    std::vector<Real> mean_variance(MatrixView covariance, std::span<const Real> linear, std::span<const Real> lower, std::span<const Real> upper,
                                    std::span<const Real> start = {});

    /// The portfolio of least variance within the bounds. Uses only the
    /// covariance: no expected return to estimate.
    std::vector<Real> minimum_variance(MatrixView covariance, std::span<const Real> lower, std::span<const Real> upper);

    /// The portfolio of highest expected excess return over volatility within
    /// the bounds (Markowitz 1952; Sharpe 1966). Throws when no admissible
    /// portfolio has a positive expected excess return.
    ///
    /// Its Sharpe ratio, measured on the data the inputs were estimated from,
    /// is a maximum taken over those data: biased upwards, and no forecast
    /// (companion, section 6.1).
    std::vector<Real> maximum_sharpe(MatrixView covariance, std::span<const Real> expected_excess, std::span<const Real> lower, std::span<const Real> upper);

    /// Equal risk contribution: Maillard, Roncalli and Teiletche, "The
    /// Properties of Equally Weighted Risk Contribution Portfolios", Journal
    /// of Portfolio Management 36(4), 2010. Long only, every position carries
    /// the same share of the variance. It takes no bounds: a bounded portfolio
    /// does not have equal contributions.
    std::vector<Real> risk_parity(MatrixView covariance);

} // namespace quantBacktest

#endif
