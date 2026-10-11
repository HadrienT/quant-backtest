#ifndef QUANTBACKTEST_VALIDATION_OVERFITTING_HPP
#define QUANTBACKTEST_VALIDATION_OVERFITTING_HPP

#include "quantBacktest/core/types.hpp"

#include <vector>

namespace quantBacktest
{

    // Correcting a Sharpe ratio for the number of trials (companion, sections
    // 9.1 and 9.3). Sharpe ratios are per period here, not annualised.

    /// Expected maximum of the Sharpe ratios of `trials` independent
    /// strategies of true Sharpe ratio zero, when the Sharpe ratios estimated
    /// across trials have standard deviation `sharpe_deviation`. Bailey and
    /// Lopez de Prado, "The Deflated Sharpe Ratio", Journal of Portfolio
    /// Management 40(5), 2014:
    ///
    ///     sigma [(1 - g) Phi^-1(1 - 1/N) + g Phi^-1(1 - 1/(N e))]
    ///
    /// with g the Euler-Mascheroni constant. Zero for a single trial.
    Real expected_maximum_sharpe(Index trials, Real sharpe_deviation);

    /// Deflated Sharpe Ratio, same reference: the probabilistic Sharpe ratio
    /// (inference/sharpe_inference.hpp) at the threshold above. It is the
    /// probability that the Sharpe ratio retained is positive, given that
    /// `trials` were made.
    Real deflated_sharpe_ratio(Real sharpe, Real skewness, Real kurtosis, Index observations, Index trials, Real sharpe_deviation);

    struct DeflatedSharpe
    {
        /// The configuration with the highest Sharpe ratio, that Sharpe ratio
        /// (per period), and the threshold it is tested against.
        Index best = 0;
        Real sharpe = 0.0;
        Real expected_maximum = 0.0;
        /// Number of trials (the columns), and standard deviation of their
        /// Sharpe ratios.
        Index trials = 0;
        Real sharpe_deviation = 0.0;
        /// The probability itself.
        Real deflated_sharpe_ratio = 0.0;
    };

    /// The deflated Sharpe ratio of the best column of a table of returns
    /// (one row per period, one column per configuration tried), every column
    /// counting as one trial. Correlated configurations are fewer independent
    /// trials than their number: the result is then conservative.
    DeflatedSharpe deflated_sharpe_of_best(MatrixView returns);

    struct BacktestOverfitting
    {
        /// Share of the splits in which the configuration that is best
        /// in-sample ranks below the median out-of-sample. Near one half on
        /// returns without any signal; near zero when one configuration is
        /// really better.
        ///
        /// It is a frequency over splits that share their data: no standard
        /// error is attached to it, and `logits` shows the whole distribution.
        Real probability = 0.0;
        Index splits = 0;
        /// For each split, ln(w / (1 - w)) with w the relative rank
        /// out-of-sample of the in-sample champion (0 worst, 1 best).
        std::vector<Real> logits;
        /// Mean, over the splits, of the champion's Sharpe ratio per period
        /// in-sample and out-of-sample: the performance lost by choosing.
        Real mean_in_sample_sharpe = 0.0;
        Real mean_out_of_sample_sharpe = 0.0;
    };

    /// Probability of backtest overfitting by combinatorially symmetric
    /// cross-validation: Bailey, Borwein, Lopez de Prado and Zhu, "The
    /// Probability of Backtest Overfitting", Journal of Computational Finance
    /// 20(4), 2017. The rows are cut into `blocks` blocks (an even number,
    /// at most 20); every way of taking half of them as in-sample is a split.
    /// Rows beyond a multiple of `blocks` are dropped from the end.
    BacktestOverfitting probability_of_backtest_overfitting(MatrixView returns, Index blocks);

} // namespace quantBacktest

#endif
