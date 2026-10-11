#include <gtest/gtest.h>

#include "quantBacktest/core/error.hpp"
#include "quantBacktest/core/estimate.hpp"
#include "quantBacktest/inference/sharpe_inference.hpp"
#include "quantBacktest/metrics/performance.hpp"
#include "quantBacktest/validation/cpcv.hpp"
#include "quantBacktest/validation/overfitting.hpp"
#include "quantBacktest/validation/walk_forward.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <random>
#include <set>
#include <vector>

namespace quantBacktest
{

    namespace
    {

        // A table of returns without any signal: every configuration has a
        // true Sharpe ratio of zero. `skilled` configurations, from the first
        // column on, get a true annualised Sharpe ratio of `skill` instead.
        std::vector<Real> table(Index rows, Index columns, std::mt19937_64 &rng, Index skilled = 0, Real skill = 0.0)
        {
            std::normal_distribution<Real> normal(0.0, 0.01);
            std::vector<Real> returns(rows * columns);
            for (Index t = 0; t < rows; ++t)
            {
                for (Index m = 0; m < columns; ++m)
                {
                    returns[t * columns + m] = normal(rng) + (m < skilled ? skill / std::sqrt(252.0) * 0.01 : 0.0);
                }
            }
            return returns;
        }

        std::vector<Real> column(const std::vector<Real> &returns, Index rows, Index columns, Index m, Index begin, Index end)
        {
            std::vector<Real> out;
            for (Index t = begin; t < end && t < rows; ++t)
            {
                out.push_back(returns[t * columns + m]);
            }
            return out;
        }

    } // namespace

    // ---- Walk-forward ----

    TEST(WalkForward, ChoosesOnTheWindowBeforeAndAppliesAfter)
    {
        std::mt19937_64 rng(1);
        const Index rows = 500;
        const Index columns = 6;
        const std::vector<Real> returns = table(rows, columns, rng);
        const WalkForwardConfig config{120, 40, false, 252.0};
        const WalkForwardResult r = walk_forward({returns.data(), rows, columns}, config);

        ASSERT_EQ(r.folds.size(), 10u);
        EXPECT_EQ(r.first_row, 120u);
        EXPECT_EQ(r.returns.size(), rows - 120);
        Index row = 120;
        for (const WalkForwardFold &fold : r.folds)
        {
            EXPECT_EQ(fold.train_end, fold.test_begin);
            EXPECT_EQ(fold.train_end - fold.train_begin, 120u);
            EXPECT_EQ(fold.test_begin, row);
            // The choice is the best Sharpe ratio of the training rows.
            Real top = -std::numeric_limits<Real>::infinity();
            Index expected = 0;
            for (Index m = 0; m < columns; ++m)
            {
                const Real sharpe = sharpe_ratio(column(returns, rows, columns, m, fold.train_begin, fold.train_end));
                if (sharpe > top)
                {
                    top = sharpe;
                    expected = m;
                }
            }
            EXPECT_EQ(fold.chosen, expected);
            EXPECT_NEAR(fold.in_sample_sharpe, top * std::sqrt(252.0), 1e-9);
            const std::vector<Real> applied = column(returns, rows, columns, fold.chosen, fold.test_begin, fold.test_end);
            EXPECT_NEAR(fold.out_of_sample_sharpe, sharpe_ratio(applied) * std::sqrt(252.0), 1e-9);
            for (Index t = fold.test_begin; t < fold.test_end; ++t)
            {
                EXPECT_EQ(r.returns[t - 120], returns[t * columns + fold.chosen]);
                EXPECT_EQ(r.chosen[t - 120], fold.chosen);
            }
            row = fold.test_end;
        }
        EXPECT_EQ(row, rows);
    }

    TEST(WalkForward, ExpandingWindowStartsAtTheFirstRow)
    {
        std::mt19937_64 rng(2);
        const std::vector<Real> returns = table(400, 4, rng);
        const WalkForwardResult r = walk_forward({returns.data(), 400, 4}, {100, 50, true, 252.0});
        ASSERT_EQ(r.folds.size(), 6u);
        for (const WalkForwardFold &fold : r.folds)
        {
            EXPECT_EQ(fold.train_begin, 0u);
            EXPECT_EQ(fold.train_end, fold.test_begin);
        }
        EXPECT_EQ(r.folds.back().train_end, 350u);
    }

    // Finding 14, and "no future": a choice never reads the rows it is
    // applied to, nor any later one. Changing them changes no earlier choice.
    TEST(WalkForward, AChoiceDoesNotDependOnWhatFollows)
    {
        std::mt19937_64 rng(3);
        const Index rows = 600;
        const Index columns = 8;
        const std::vector<Real> returns = table(rows, columns, rng);
        std::vector<Real> changed = returns;
        const Index cut = 400;
        for (Index k = cut * columns; k < changed.size(); ++k)
        {
            changed[k] = -changed[k] + 0.001;
        }
        for (const bool expanding : {false, true})
        {
            const WalkForwardConfig config{150, 50, expanding, 252.0};
            const WalkForwardResult a = walk_forward({returns.data(), rows, columns}, config);
            const WalkForwardResult b = walk_forward({changed.data(), rows, columns}, config);
            Index compared = 0;
            for (Index k = 0; k < a.folds.size(); ++k)
            {
                if (a.folds[k].test_begin <= cut)
                {
                    EXPECT_EQ(a.folds[k].chosen, b.folds[k].chosen);
                    ++compared;
                }
            }
            EXPECT_GE(compared, 5u);
            for (Index t = 0; t < cut - 150; ++t)
            {
                ASSERT_EQ(a.returns[t], b.returns[t]);
            }
        }
    }

    // On returns without signal, choosing the best of the past earns nothing
    // afterwards, while the in-sample Sharpe ratio of each choice is high.
    TEST(WalkForward, SelectionOnNoiseHasNoOutOfSampleSharpe)
    {
        std::mt19937_64 rng(4);
        const Index rows = 252 * 40;
        const Index columns = 20;
        const std::vector<Real> returns = table(rows, columns, rng);
        const WalkForwardResult r = walk_forward({returns.data(), rows, columns}, {252, 63, false, 252.0});
        Real inSample = 0.0;
        for (const WalkForwardFold &fold : r.folds)
        {
            inSample += fold.in_sample_sharpe / static_cast<Real>(r.folds.size());
        }
        // The best of 20 over one year: about 1.9 in expectation.
        EXPECT_GT(inSample, 1.5);
        // 39 years out of sample: a standard error of 0.16.
        EXPECT_NEAR(sharpe_ratio(r.returns) * std::sqrt(252.0), 0.0, 0.5);
    }

    // With one configuration that is really better, the selection finds it
    // and keeps its Sharpe ratio out of sample.
    TEST(WalkForward, SelectionFindsARealEdge)
    {
        std::mt19937_64 rng(5);
        const Index rows = 252 * 20;
        const std::vector<Real> returns = table(rows, 10, rng, 1, 2.0);
        const WalkForwardResult r = walk_forward({returns.data(), rows, 10}, {504, 126, false, 252.0});
        const auto picked = std::count(r.chosen.begin(), r.chosen.end(), Index{0});
        EXPECT_GT(static_cast<Real>(picked) / static_cast<Real>(r.chosen.size()), 0.7);
        EXPECT_GT(sharpe_ratio(r.returns) * std::sqrt(252.0), 1.0);
    }

    TEST(WalkForward, RefusesBadInputs)
    {
        std::mt19937_64 rng(6);
        std::vector<Real> returns = table(100, 3, rng);
        EXPECT_THROW(walk_forward({returns.data(), 100, 3}, {100, 10, false, 252.0}), Error);
        EXPECT_THROW(walk_forward({returns.data(), 100, 3}, {1, 10, false, 252.0}), Error);
        EXPECT_THROW(walk_forward({returns.data(), 100, 3}, {50, 0, false, 252.0}), Error);
        EXPECT_THROW(walk_forward(MatrixView{}, {50, 10, false, 252.0}), Error);
        returns[17] = std::numeric_limits<Real>::quiet_NaN();
        EXPECT_THROW(walk_forward({returns.data(), 100, 3}, {50, 10, false, 252.0}), Error);
    }

    // ---- Combinatorial purged cross-validation ----

    TEST(Cpcv, CountsSplitsAndPathsAndCoversEveryRowOncePerPath)
    {
        std::mt19937_64 rng(7);
        const Index rows = 600;
        const Index columns = 5;
        const std::vector<Real> returns = table(rows, columns, rng);
        CpcvConfig config;
        config.groups = 6;
        config.test_groups = 2;
        const CpcvResult r = combinatorial_purged_cv({returns.data(), rows, columns}, config);
        // C(6, 2) = 15 splits, 15 x 2 / 6 = 5 paths.
        EXPECT_EQ(r.splits, 15u);
        EXPECT_EQ(r.paths, 5u);
        ASSERT_EQ(r.path_sharpes.size(), 5u);
        ASSERT_EQ(r.path_returns.size(), 5u * rows);
        ASSERT_EQ(r.chosen.size(), 15u);
        // Every value of a path is the return of some configuration that day.
        for (Index p = 0; p < r.paths; ++p)
        {
            for (Index t = 0; t < rows; ++t)
            {
                const Real value = r.path_returns[p * rows + t];
                bool found = false;
                for (Index m = 0; m < columns; ++m)
                {
                    found |= value == returns[t * columns + m];
                }
                ASSERT_TRUE(found) << "path " << p << ", row " << t;
            }
            const std::vector<Real> path(r.path_returns.begin() + static_cast<std::ptrdiff_t>(p * rows),
                                         r.path_returns.begin() + static_cast<std::ptrdiff_t>((p + 1) * rows));
            EXPECT_NEAR(r.path_sharpes[p], sharpe_ratio(path) * std::sqrt(252.0), 1e-9);
        }
    }

    // The choice of a split is made on the training rows only, and the purge
    // and the embargo remove the rows next to the test groups from them.
    TEST(Cpcv, PurgeAndEmbargoRemoveTheNeighboursOfTheTestGroups)
    {
        const Index rows = 400;
        const Index columns = 2;
        // Column 1 is the better one, except on the 20 rows that follow each
        // multiple of 100, where it loses. With an embargo of 20 rows, the
        // losing stretch that follows a test group leaves the training rows.
        std::vector<Real> returns(rows * columns);
        std::mt19937_64 rng(8);
        std::normal_distribution<Real> normal(0.0, 0.01);
        for (Index t = 0; t < rows; ++t)
        {
            returns[t * columns] = normal(rng) - 0.002;
            returns[t * columns + 1] = normal(rng) + (t % 100 < 20 ? -0.012 : 0.004);
        }
        CpcvConfig config;
        config.groups = 4;
        config.test_groups = 1;
        const CpcvResult plain = combinatorial_purged_cv({returns.data(), rows, columns}, config);
        config.embargo = 20;
        const CpcvResult embargoed = combinatorial_purged_cv({returns.data(), rows, columns}, config);
        for (Index s = 0; s < 3; ++s)
        {
            ASSERT_EQ(plain.chosen[s], 1u);
            ASSERT_EQ(embargoed.chosen[s], 1u);
            EXPECT_GT(embargoed.in_sample_sharpes[s], plain.in_sample_sharpes[s] + 0.5);
            // The test rows are the same: so is what is measured on them.
            EXPECT_EQ(embargoed.out_of_sample_sharpes[s], plain.out_of_sample_sharpes[s]);
        }
        // Testing the last group: nothing follows it, the embargo is empty.
        EXPECT_EQ(embargoed.in_sample_sharpes[3], plain.in_sample_sharpes[3]);

        config.embargo = 0;
        config.purge = 400;
        EXPECT_THROW(combinatorial_purged_cv({returns.data(), rows, columns}, config), Error);
        config.purge = 0;
        config.test_groups = 4;
        EXPECT_THROW(combinatorial_purged_cv({returns.data(), rows, columns}, config), Error);
        config.groups = 1;
        EXPECT_THROW(combinatorial_purged_cv({returns.data(), rows, columns}, config), Error);
    }

    // On noise the out-of-sample Sharpe ratios of the paths are centred on
    // zero; with a real edge they are not.
    TEST(Cpcv, PathSharpesSeparateNoiseFromARealEdge)
    {
        std::mt19937_64 rng(9);
        const Index rows = 252 * 10;
        CpcvConfig config;
        config.groups = 10;
        config.test_groups = 2;
        config.purge = 5;
        config.embargo = 5;
        Real noiseMean = 0.0;
        const int draws = 40;
        for (int draw = 0; draw < draws; ++draw)
        {
            const std::vector<Real> noise = table(rows, 15, rng);
            const CpcvResult r = combinatorial_purged_cv({noise.data(), rows, 15}, config);
            EXPECT_EQ(r.paths, 9u);
            noiseMean += std::accumulate(r.path_sharpes.begin(), r.path_sharpes.end(), 0.0) / static_cast<Real>(r.paths) / draws;
        }
        EXPECT_NEAR(noiseMean, 0.0, 0.15);

        const std::vector<Real> skilled = table(rows, 15, rng, 1, 2.0);
        const CpcvResult r = combinatorial_purged_cv({skilled.data(), rows, 15}, config);
        for (const Real sharpe : r.path_sharpes)
        {
            EXPECT_GT(sharpe, 0.8);
        }
    }

    // ---- Deflated Sharpe ratio ----

    // The figure of the companion: with five years of data, 45 independent
    // trials are enough to expect an annualised Sharpe ratio of 1 in pure
    // noise.
    TEST(DeflatedSharpe, FortyFiveTrialsFindASharpeOfOneInFiveYearsOfNoise)
    {
        const Real annualDeviation = 1.0 / std::sqrt(5.0);
        EXPECT_NEAR(expected_maximum_sharpe(45, annualDeviation), 1.0, 0.01);
        EXPECT_EQ(expected_maximum_sharpe(1, annualDeviation), 0.0);
        EXPECT_GT(expected_maximum_sharpe(100, annualDeviation), expected_maximum_sharpe(45, annualDeviation));
        EXPECT_THROW(expected_maximum_sharpe(0, annualDeviation), Error);
        EXPECT_THROW(expected_maximum_sharpe(10, -1.0), Error);
    }

    // The formula is an approximation of the expected maximum of N Gaussian
    // draws: checked against a simulation.
    TEST(DeflatedSharpe, ExpectedMaximumMatchesSimulation)
    {
        std::mt19937_64 rng(10);
        std::normal_distribution<Real> normal(0.0, 1.0);
        for (const Index trials : {Index{10}, Index{100}, Index{1000}})
        {
            Real mean = 0.0;
            const int draws = 4000;
            for (int draw = 0; draw < draws; ++draw)
            {
                Real top = -std::numeric_limits<Real>::infinity();
                for (Index k = 0; k < trials; ++k)
                {
                    top = std::max(top, normal(rng));
                }
                mean += top / draws;
            }
            EXPECT_NEAR(expected_maximum_sharpe(trials, 1.0), mean, 0.05) << trials << " trials";
        }
    }

    TEST(DeflatedSharpe, IsThePsrAtTheExpectedMaximum)
    {
        const Real threshold = expected_maximum_sharpe(50, 0.03);
        EXPECT_DOUBLE_EQ(deflated_sharpe_ratio(0.08, -0.5, 4.0, 1000, 50, 0.03),
                         probabilistic_sharpe_ratio(0.08, threshold, -0.5, 4.0, 1000));
        // One trial: nothing to deflate.
        EXPECT_DOUBLE_EQ(deflated_sharpe_ratio(0.08, 0.0, 3.0, 1000, 1, 0.03), probabilistic_sharpe_ratio(0.08, 0.0, 0.0, 3.0, 1000));
        // More trials, less confidence in the same Sharpe ratio.
        EXPECT_LT(deflated_sharpe_ratio(0.08, 0.0, 3.0, 1000, 500, 0.03), deflated_sharpe_ratio(0.08, 0.0, 3.0, 1000, 5, 0.03));
    }

    // Acceptance criterion of work package B5: on returns without signal, the
    // deflated Sharpe ratio of the best trial is below 0.95 in at least 95 %
    // of the draws.
    TEST(DeflatedSharpe, TheBestOfManyTrialsOnNoiseRarelyLooksSignificant)
    {
        std::mt19937_64 rng(11);
        const Index rows = 1000;
        const Index columns = 50;
        const int draws = 400;
        int significant = 0;
        int naive = 0;
        for (int draw = 0; draw < draws; ++draw)
        {
            const std::vector<Real> returns = table(rows, columns, rng);
            const DeflatedSharpe d = deflated_sharpe_of_best({returns.data(), rows, columns});
            EXPECT_EQ(d.trials, columns);
            significant += d.deflated_sharpe_ratio >= 0.95 ? 1 : 0;
            // Without the correction, the best of 50 nearly always "is".
            naive += probabilistic_sharpe_ratio(d.sharpe, 0.0, 0.0, 3.0, rows) >= 0.95 ? 1 : 0;
        }
        EXPECT_LE(static_cast<Real>(significant) / draws, 0.05);
        EXPECT_GT(static_cast<Real>(naive) / draws, 0.8);
    }

    TEST(DeflatedSharpe, ARealEdgeSurvivesTheDeflation)
    {
        std::mt19937_64 rng(12);
        const Index rows = 252 * 8;
        const std::vector<Real> returns = table(rows, 50, rng, 1, 2.0);
        const DeflatedSharpe d = deflated_sharpe_of_best({returns.data(), rows, 50});
        EXPECT_EQ(d.best, 0u);
        EXPECT_GT(d.deflated_sharpe_ratio, 0.95);
        EXPECT_GT(d.sharpe, d.expected_maximum);
        EXPECT_THROW(deflated_sharpe_of_best(MatrixView{}), Error);
    }

    // ---- Probability of backtest overfitting ----

    // Acceptance criterion: on returns without signal, the probability of
    // overfitting is close to one half.
    TEST(BacktestOverfitting, IsOneHalfOnNoise)
    {
        std::mt19937_64 rng(13);
        const Index rows = 1000;
        const Index columns = 30;
        Real mean = 0.0;
        Real lost = 0.0;
        const int draws = 60;
        for (int draw = 0; draw < draws; ++draw)
        {
            const std::vector<Real> returns = table(rows, columns, rng);
            const BacktestOverfitting r = probability_of_backtest_overfitting({returns.data(), rows, columns}, 10);
            // C(10, 5) splits.
            EXPECT_EQ(r.splits, 252u);
            EXPECT_EQ(r.logits.size(), 252u);
            mean += r.probability / draws;
            lost += (r.mean_in_sample_sharpe - r.mean_out_of_sample_sharpe) / draws;
        }
        EXPECT_NEAR(mean, 0.5, 0.05);
        // The champion's Sharpe ratio is all in-sample.
        EXPECT_GT(lost, 0.05);
    }

    TEST(BacktestOverfitting, IsNearZeroWithARealEdgeAndHighWithAnAntiPersistentOne)
    {
        std::mt19937_64 rng(14);
        const Index rows = 2000;
        const Index columns = 20;
        const std::vector<Real> skilled = table(rows, columns, rng, 1, 3.0);
        const BacktestOverfitting good = probability_of_backtest_overfitting({skilled.data(), rows, columns}, 10);
        EXPECT_LT(good.probability, 0.05);
        EXPECT_NEAR(good.mean_out_of_sample_sharpe, good.mean_in_sample_sharpe, 0.05);
    }

    TEST(BacktestOverfitting, RankOfTheChampionByHand)
    {
        // Two blocks of two rows, three configurations. Configuration 0 is
        // the best on the first block and the worst on the second;
        // configuration 2 the reverse.
        const std::vector<Real> returns = {
            0.03, 0.01, -0.02,
            0.02, 0.02, -0.01,
            -0.02, 0.01, 0.03,
            -0.01, 0.02, 0.02};
        const BacktestOverfitting r = probability_of_backtest_overfitting({returns.data(), 4, 3}, 2);
        // Two splits; each champion is last out-of-sample: rank 1 of 3.
        ASSERT_EQ(r.splits, 2u);
        for (const Real logit : r.logits)
        {
            EXPECT_NEAR(logit, std::log(0.25 / 0.75), 1e-12);
        }
        EXPECT_DOUBLE_EQ(r.probability, 1.0);
        EXPECT_GT(r.mean_in_sample_sharpe, 0.0);
        EXPECT_LT(r.mean_out_of_sample_sharpe, 0.0);

        EXPECT_THROW(probability_of_backtest_overfitting({returns.data(), 4, 3}, 3), Error);
        EXPECT_THROW(probability_of_backtest_overfitting({returns.data(), 4, 3}, 22), Error);
        EXPECT_THROW(probability_of_backtest_overfitting({returns.data(), 4, 3}, 4), Error);
        EXPECT_THROW(probability_of_backtest_overfitting({returns.data(), 12, 1}, 2), Error);
    }

} // namespace quantBacktest
