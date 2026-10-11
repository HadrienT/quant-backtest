#ifndef QUANTBACKTEST_VALIDATION_CPCV_HPP
#define QUANTBACKTEST_VALIDATION_CPCV_HPP

#include "quantBacktest/core/types.hpp"

#include <vector>

namespace quantBacktest
{

    struct CpcvConfig
    {
        /// Number of contiguous groups the rows are cut into, and number of
        /// them held out for testing in each split.
        Index groups = 0;
        Index test_groups = 0;
        /// Rows removed from the training set on each side of a test group,
        /// because their estimation window overlaps it: at least the lookback
        /// of the strategies compared.
        Index purge = 0;
        /// Further rows removed after each test group.
        Index embargo = 0;
        Real periods_per_year = 252.0;
    };

    struct CpcvResult
    {
        /// Number of splits, C(groups, test_groups), and of out-of-sample
        /// paths they rebuild, C(groups, test_groups) x test_groups / groups.
        Index splits = 0;
        Index paths = 0;
        /// Annualised Sharpe ratio of each rebuilt path: a **distribution** of
        /// out-of-sample Sharpe ratios instead of one number. The paths share
        /// their data: they are not independent draws.
        std::vector<Real> path_sharpes;
        /// Returns of each path, one row per path and one column per period.
        std::vector<Real> path_returns;
        /// For each split, the annualised Sharpe ratio of the configuration
        /// chosen, on the training rows and on the test rows.
        std::vector<Real> in_sample_sharpes;
        std::vector<Real> out_of_sample_sharpes;
        std::vector<Index> chosen;
    };

    /// Combinatorial purged cross-validation: Lopez de Prado, "Advances in
    /// Financial Machine Learning", 2018, chapters 7 and 12. For every way of
    /// holding `test_groups` groups out of `groups`, the configuration with
    /// the highest Sharpe ratio on the other rows (purged and embargoed) is
    /// applied to the held-out groups; the held-out pieces are then assembled
    /// into full-length paths.
    ///
    /// Same table as `walk_forward`. Unlike it, a split trains on rows that
    /// come after its test rows: it measures the stability of the selection,
    /// it is not a simulation of what could have been traded.
    CpcvResult combinatorial_purged_cv(MatrixView returns, const CpcvConfig &config);

} // namespace quantBacktest

#endif
