#ifndef QUANTBACKTEST_VALIDATION_WALK_FORWARD_HPP
#define QUANTBACKTEST_VALIDATION_WALK_FORWARD_HPP

#include "quantBacktest/core/types.hpp"

#include <vector>

namespace quantBacktest
{

    // Separating what chooses from what measures (companion, section 9.2).
    //
    // The functions of this module work on a table of returns with one row
    // per period and one column per **configuration**: each column is the
    // return series of one backtest (one window, one threshold, one list of
    // assets...). Every configuration that was tried belongs in the table,
    // the abandoned ones included: their number is the number of trials.

    struct WalkForwardConfig
    {
        /// Periods used to choose a configuration.
        Index train = 0;
        /// Periods the chosen configuration is then applied to.
        Index test = 0;
        /// False: the training window slides and keeps `train` periods. True:
        /// it grows, from the first period.
        bool expanding = false;
        Real periods_per_year = 252.0;
    };

    struct WalkForwardFold
    {
        /// Training rows [train_begin, train_end) and test rows
        /// [test_begin, test_end): the test follows the training.
        Index train_begin = 0;
        Index train_end = 0;
        Index test_begin = 0;
        Index test_end = 0;
        /// The configuration with the highest Sharpe ratio on the training
        /// rows, and that Sharpe ratio, annualised.
        Index chosen = 0;
        Real in_sample_sharpe = 0.0;
        /// Its annualised Sharpe ratio on the test rows; NaN if the test rows
        /// have no volatility.
        Real out_of_sample_sharpe = 0.0;
    };

    struct WalkForwardResult
    {
        std::vector<WalkForwardFold> folds;
        /// The returns of the configuration chosen for each fold, on its test
        /// rows, end to end: the one honest reading of "re-optimised at each
        /// rebalancing". It starts at row `first_row` of the table: the first
        /// `train` periods are never tested.
        Index first_row = 0;
        std::vector<Real> returns;
        /// Configuration held on each of those rows.
        std::vector<Index> chosen;
    };

    /// Walk-forward selection (finding 14): at each step the configuration is
    /// chosen on the window that ends there, never on a single window picked
    /// by the user, and applied until the next step. A choice only reads rows
    /// before the ones it is applied to.
    ///
    /// `returns` are excess returns if the Sharpe ratio is to be an excess
    /// one. Throws when a return is missing or when the table is shorter than
    /// one training window and one test period.
    WalkForwardResult walk_forward(MatrixView returns, const WalkForwardConfig &config);

} // namespace quantBacktest

#endif
