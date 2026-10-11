#include "quantBacktest/validation/cpcv.hpp"
#include "quantBacktest/validation/overfitting.hpp"
#include "quantBacktest/validation/walk_forward.hpp"

#include "quantBacktest/core/error.hpp"
#include "quantBacktest/core/estimate.hpp"
#include "quantBacktest/inference/sharpe_inference.hpp"
#include "quantBacktest/metrics/performance.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <string>

namespace quantBacktest
{

    namespace
    {

        constexpr Real kNaN = std::numeric_limits<Real>::quiet_NaN();
        constexpr Real kEulerMascheroni = 0.5772156649015329;

        void requireTable(MatrixView returns, Index minimumRows)
        {
            if (returns.empty() || returns.cols() == 0)
            {
                throw Error("the table of returns needs at least one configuration");
            }
            if (returns.rows() < minimumRows)
            {
                throw Error("the table of returns needs at least " + std::to_string(minimumRows) + " periods");
            }
            for (Index k = 0; k < returns.rows() * returns.cols(); ++k)
            {
                if (!std::isfinite(returns.data()[k]))
                {
                    throw Error("the table of returns holds a missing or infinite value");
                }
            }
        }

        // Sums of a column over a set of rows: enough for its Sharpe ratio.
        struct Moments
        {
            Real sum = 0.0;
            Real squares = 0.0;
            Index count = 0;

            void add(Real x)
            {
                sum += x;
                squares += x * x;
                ++count;
            }

            void add(const Moments &other)
            {
                sum += other.sum;
                squares += other.squares;
                count += other.count;
            }

            // Per period; NaN without volatility or with fewer than 2 rows.
            Real sharpe() const
            {
                if (count < 2)
                {
                    return kNaN;
                }
                const Real n = static_cast<Real>(count);
                const Real mean = sum / n;
                const Real variance = (squares - n * mean * mean) / (n - 1.0);
                return variance > 1e-24 ? mean / std::sqrt(variance) : kNaN;
            }
        };

        Moments momentsOf(MatrixView returns, Index column, Index begin, Index end)
        {
            Moments m;
            for (Index t = begin; t < end; ++t)
            {
                m.add(returns(t, column));
            }
            return m;
        }

        // The column with the highest Sharpe ratio; ties and undefined ratios
        // go to the lowest index, so that the choice is deterministic.
        Index best(const std::vector<Real> &sharpes)
        {
            Index chosen = 0;
            Real top = -std::numeric_limits<Real>::infinity();
            for (Index m = 0; m < sharpes.size(); ++m)
            {
                if (!is_missing(sharpes[m]) && sharpes[m] > top)
                {
                    top = sharpes[m];
                    chosen = m;
                }
            }
            return chosen;
        }

        // Every way of choosing `take` of `from` items, in lexicographic order.
        std::vector<std::vector<Index>> combinations(Index from, Index take)
        {
            std::vector<std::vector<Index>> out;
            std::vector<Index> current(take);
            for (Index k = 0; k < take; ++k)
            {
                current[k] = k;
            }
            for (;;)
            {
                out.push_back(current);
                Index position = take;
                while (position > 0 && current[position - 1] == from - take + position - 1)
                {
                    --position;
                }
                if (position == 0)
                {
                    return out;
                }
                ++current[position - 1];
                for (Index k = position; k < take; ++k)
                {
                    current[k] = current[k - 1] + 1;
                }
            }
        }

    } // namespace

    // ---- Walk-forward ----

    WalkForwardResult walk_forward(MatrixView returns, const WalkForwardConfig &config)
    {
        if (config.train < 2 || config.test == 0)
        {
            throw Error("a walk-forward needs a training window of at least 2 periods and a test window of at least 1");
        }
        if (!(config.periods_per_year > 0.0))
        {
            throw Error("the number of periods per year must be positive");
        }
        requireTable(returns, config.train + 1);
        const Index rows = returns.rows();
        const Index columns = returns.cols();
        const Real annual = std::sqrt(config.periods_per_year);

        WalkForwardResult out;
        out.first_row = config.train;
        std::vector<Real> sharpes(columns);
        for (Index testBegin = config.train; testBegin < rows; testBegin += config.test)
        {
            WalkForwardFold fold;
            fold.train_end = testBegin;
            fold.train_begin = config.expanding ? 0 : testBegin - config.train;
            fold.test_begin = testBegin;
            fold.test_end = std::min(testBegin + config.test, rows);
            for (Index m = 0; m < columns; ++m)
            {
                sharpes[m] = momentsOf(returns, m, fold.train_begin, fold.train_end).sharpe();
            }
            fold.chosen = best(sharpes);
            fold.in_sample_sharpe = sharpes[fold.chosen] * annual;
            fold.out_of_sample_sharpe = momentsOf(returns, fold.chosen, fold.test_begin, fold.test_end).sharpe() * annual;
            for (Index t = fold.test_begin; t < fold.test_end; ++t)
            {
                out.returns.push_back(returns(t, fold.chosen));
                out.chosen.push_back(fold.chosen);
            }
            out.folds.push_back(fold);
        }
        return out;
    }

    // ---- Combinatorial purged cross-validation ----

    CpcvResult combinatorial_purged_cv(MatrixView returns, const CpcvConfig &config)
    {
        if (config.groups < 2 || config.test_groups == 0 || config.test_groups >= config.groups)
        {
            throw Error("the cross-validation needs at least 2 groups, and between 1 and all but one of them for testing");
        }
        if (config.groups > 30)
        {
            throw Error("the cross-validation is limited to 30 groups");
        }
        if (!(config.periods_per_year > 0.0))
        {
            throw Error("the number of periods per year must be positive");
        }
        requireTable(returns, 2 * config.groups);
        const Index rows = returns.rows();
        const Index columns = returns.cols();
        const Index groups = config.groups;
        const Real annual = std::sqrt(config.periods_per_year);

        // Group g holds the rows [bounds[g], bounds[g + 1]).
        std::vector<Index> bounds(groups + 1);
        for (Index g = 0; g <= groups; ++g)
        {
            bounds[g] = g * rows / groups;
        }

        const std::vector<std::vector<Index>> splits = combinations(groups, config.test_groups);
        CpcvResult out;
        out.splits = splits.size();
        out.paths = splits.size() * config.test_groups / groups;
        out.path_returns.assign(out.paths * rows, 0.0);
        std::vector<Index> used(groups, 0);
        std::vector<char> training(rows);
        std::vector<Real> sharpes(columns);

        for (const std::vector<Index> &testGroups : splits)
        {
            std::fill(training.begin(), training.end(), 1);
            for (const Index g : testGroups)
            {
                // The test rows, the purge on both sides, the embargo after.
                const Index from = bounds[g] >= config.purge ? bounds[g] - config.purge : 0;
                const Index to = std::min(bounds[g + 1] + config.purge + config.embargo, rows);
                std::fill(training.begin() + static_cast<std::ptrdiff_t>(from), training.begin() + static_cast<std::ptrdiff_t>(to), 0);
            }
            for (Index m = 0; m < columns; ++m)
            {
                Moments moments;
                for (Index t = 0; t < rows; ++t)
                {
                    if (training[t] != 0)
                    {
                        moments.add(returns(t, m));
                    }
                }
                sharpes[m] = moments.sharpe();
            }
            const Index chosen = best(sharpes);
            if (is_missing(sharpes[chosen]))
            {
                throw Error("the cross-validation has no training rows left for a split: fewer test groups, or a shorter purge and embargo");
            }
            Moments test;
            for (const Index g : testGroups)
            {
                test.add(momentsOf(returns, chosen, bounds[g], bounds[g + 1]));
                // The k-th time a group is tested, it goes to path k.
                const Index path = used[g]++;
                for (Index t = bounds[g]; t < bounds[g + 1]; ++t)
                {
                    out.path_returns[path * rows + t] = returns(t, chosen);
                }
            }
            out.chosen.push_back(chosen);
            out.in_sample_sharpes.push_back(sharpes[chosen] * annual);
            out.out_of_sample_sharpes.push_back(test.sharpe() * annual);
        }
        for (Index p = 0; p < out.paths; ++p)
        {
            Moments moments;
            for (Index t = 0; t < rows; ++t)
            {
                moments.add(out.path_returns[p * rows + t]);
            }
            out.path_sharpes.push_back(moments.sharpe() * annual);
        }
        return out;
    }

    // ---- Deflated Sharpe ratio ----

    Real expected_maximum_sharpe(Index trials, Real sharpe_deviation)
    {
        if (trials == 0)
        {
            throw Error("the number of trials must be at least 1");
        }
        if (!(std::isfinite(sharpe_deviation) && sharpe_deviation >= 0.0))
        {
            throw Error("the standard deviation of the Sharpe ratios across trials must be a non-negative number");
        }
        if (trials == 1)
        {
            return 0.0;
        }
        const Real n = static_cast<Real>(trials);
        return sharpe_deviation * ((1.0 - kEulerMascheroni) * normal_quantile(1.0 - 1.0 / n) +
                                   kEulerMascheroni * normal_quantile(1.0 - 1.0 / (n * std::numbers::e)));
    }

    Real deflated_sharpe_ratio(Real sharpe, Real skewness, Real kurtosis, Index observations, Index trials, Real sharpe_deviation)
    {
        return probabilistic_sharpe_ratio(sharpe, expected_maximum_sharpe(trials, sharpe_deviation), skewness, kurtosis, observations);
    }

    DeflatedSharpe deflated_sharpe_of_best(MatrixView returns)
    {
        requireTable(returns, 4);
        const Index columns = returns.cols();
        std::vector<Real> sharpes(columns);
        std::vector<Real> defined;
        for (Index m = 0; m < columns; ++m)
        {
            sharpes[m] = momentsOf(returns, m, 0, returns.rows()).sharpe();
            if (!is_missing(sharpes[m]))
            {
                defined.push_back(sharpes[m]);
            }
        }
        if (defined.empty())
        {
            throw Error("no configuration has a Sharpe ratio: none has any volatility");
        }
        DeflatedSharpe out;
        out.best = best(sharpes);
        out.sharpe = sharpes[out.best];
        out.trials = columns;
        out.sharpe_deviation = defined.size() > 1 ? standard_deviation(defined) : 0.0;
        out.expected_maximum = expected_maximum_sharpe(columns, out.sharpe_deviation);
        std::vector<Real> column(returns.rows());
        for (Index t = 0; t < returns.rows(); ++t)
        {
            column[t] = returns(t, out.best);
        }
        out.deflated_sharpe_ratio = probabilistic_sharpe_ratio(out.sharpe, out.expected_maximum, skewness(column), kurtosis(column), returns.rows());
        return out;
    }

    // ---- Probability of backtest overfitting ----

    BacktestOverfitting probability_of_backtest_overfitting(MatrixView returns, Index blocks)
    {
        if (blocks < 2 || blocks % 2 != 0 || blocks > 20)
        {
            throw Error("the number of blocks must be even, between 2 and 20");
        }
        requireTable(returns, 2 * blocks);
        const Index columns = returns.cols();
        if (columns < 2)
        {
            throw Error("the probability of overfitting needs at least 2 configurations to rank");
        }
        const Index size = returns.rows() / blocks;

        // Sums of each column over each block, once.
        std::vector<Moments> moments(blocks * columns);
        for (Index b = 0; b < blocks; ++b)
        {
            for (Index m = 0; m < columns; ++m)
            {
                moments[b * columns + m] = momentsOf(returns, m, b * size, (b + 1) * size);
            }
        }

        const std::vector<std::vector<Index>> splits = combinations(blocks, blocks / 2);
        BacktestOverfitting out;
        out.splits = splits.size();
        std::vector<char> inSample(blocks);
        std::vector<Real> trained(columns);
        std::vector<Real> tested(columns);
        Index below = 0;
        for (const std::vector<Index> &half : splits)
        {
            std::fill(inSample.begin(), inSample.end(), 0);
            for (const Index b : half)
            {
                inSample[b] = 1;
            }
            for (Index m = 0; m < columns; ++m)
            {
                Moments in;
                Moments outOf;
                for (Index b = 0; b < blocks; ++b)
                {
                    (inSample[b] != 0 ? in : outOf).add(moments[b * columns + m]);
                }
                trained[m] = in.sharpe();
                tested[m] = outOf.sharpe();
            }
            const Index champion = best(trained);
            if (is_missing(trained[champion]) || is_missing(tested[champion]))
            {
                throw Error("a configuration has no volatility on half of the blocks: the probability of overfitting cannot be computed");
            }
            // Rank of the champion out-of-sample: 1 for the worst, `columns`
            // for the best; ties count for half.
            Real rank = 1.0;
            for (Index m = 0; m < columns; ++m)
            {
                if (m == champion || is_missing(tested[m]))
                {
                    continue;
                }
                rank += tested[m] < tested[champion] ? 1.0 : (tested[m] == tested[champion] ? 0.5 : 0.0);
            }
            const Real relative = rank / static_cast<Real>(columns + 1);
            const Real logit = std::log(relative / (1.0 - relative));
            out.logits.push_back(logit);
            below += logit <= 0.0 ? 1 : 0;
            out.mean_in_sample_sharpe += trained[champion] / static_cast<Real>(splits.size());
            out.mean_out_of_sample_sharpe += tested[champion] / static_cast<Real>(splits.size());
        }
        out.probability = static_cast<Real>(below) / static_cast<Real>(splits.size());
        return out;
    }

} // namespace quantBacktest
