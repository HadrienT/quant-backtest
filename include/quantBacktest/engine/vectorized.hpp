#ifndef QUANTBACKTEST_ENGINE_VECTORIZED_HPP
#define QUANTBACKTEST_ENGINE_VECTORIZED_HPP

#include "quantBacktest/core/types.hpp"

#include <span>
#include <vector>

namespace quantBacktest
{

    /// The vectorized backtest of the companion, section 4.1: weights times
    /// returns, without a book and without costs.
    ///
    ///     V[0] = initial_capital
    ///     V[t] = V[t-1] (1 + sum_i w[t, i] R[t, i] + (1 - sum_i w[t, i]) rf[t])
    ///
    /// `weights` row t holds the weights held during day t, decided no later
    /// than t-1 (section 0): the caller is responsible for that lag. `returns`
    /// are total returns (data/returns.hpp); `cash_rate` may be empty.
    ///
    /// Throws `Error` when a non-zero weight meets a missing return.
    std::vector<Real> vectorized_values(MatrixView returns, MatrixView weights, std::span<const Real> cash_rate, Real initial_capital);

} // namespace quantBacktest

#endif
