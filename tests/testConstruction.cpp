#include <gtest/gtest.h>

#include "quantBacktest/construction/allocation_strategy.hpp"
#include "quantBacktest/construction/covariance.hpp"
#include "quantBacktest/construction/optimizer.hpp"
#include "quantBacktest/core/error.hpp"
#include "quantBacktest/core/random.hpp"
#include "quantBacktest/metrics/performance.hpp"

#include "support/synthetic_market.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <random>
#include <vector>

namespace quantBacktest
{

    namespace
    {

        // Returns with a one-factor structure: correlated, with unequal
        // volatilities and unequal means.
        std::vector<Real> factorReturns(Index periods, Index assets, std::uint64_t seed)
        {
            std::mt19937_64 rng(seed);
            std::normal_distribution<Real> normal(0.0, 1.0);
            std::uniform_real_distribution<Real> uniform(0.0, 1.0);
            std::vector<Real> beta(assets);
            std::vector<Real> specific(assets);
            std::vector<Real> drift(assets);
            for (Index i = 0; i < assets; ++i)
            {
                beta[i] = 0.5 + uniform(rng);
                specific[i] = 0.005 + 0.02 * uniform(rng);
                drift[i] = 0.001 * (uniform(rng) - 0.3);
            }
            std::vector<Real> returns(periods * assets);
            for (Index t = 0; t < periods; ++t)
            {
                const Real market = 0.01 * normal(rng);
                for (Index i = 0; i < assets; ++i)
                {
                    returns[t * assets + i] = drift[i] + beta[i] * market + specific[i] * normal(rng);
                }
            }
            return returns;
        }

        // A random portfolio within the bounds: start on the bounds, then
        // move weight between random pairs as far as the bounds allow.
        std::vector<Real> randomAdmissible(Index assets, Real lower, Real upper, Random &random)
        {
            std::vector<Real> w(assets, lower);
            Real rest = 1.0 - lower * static_cast<Real>(assets);
            for (Index i = 0; i < assets && rest > 0.0; ++i)
            {
                const Real add = std::min(upper - lower, rest);
                w[i] += add;
                rest -= add;
            }
            for (Index k = 0; k < 20 * assets; ++k)
            {
                const Index from = random.uniform_index(assets);
                const Index to = random.uniform_index(assets);
                const Real amount = random.uniform() * std::min(w[from] - lower, upper - w[to]);
                w[from] -= amount;
                w[to] += amount;
            }
            return w;
        }

        Real sum(const std::vector<Real> &w)
        {
            return std::accumulate(w.begin(), w.end(), 0.0);
        }

    } // namespace

    // ---- Covariance ----

    TEST(Covariance, SampleByHand)
    {
        const std::vector<Real> returns = {1.0, 2.0,
                                           3.0, 2.0,
                                           5.0, 8.0};
        const Covariance c = sample_covariance({returns.data(), 3, 2});
        EXPECT_DOUBLE_EQ(c.matrix[0], 4.0);
        EXPECT_DOUBLE_EQ(c.matrix[1], 6.0);
        EXPECT_DOUBLE_EQ(c.matrix[2], 6.0);
        EXPECT_DOUBLE_EQ(c.matrix[3], 12.0);
        EXPECT_EQ(c.shrinkage, 0.0);
        const std::vector<Real> mean = mean_returns({returns.data(), 3, 2});
        EXPECT_DOUBLE_EQ(mean[0], 3.0);
        EXPECT_DOUBLE_EQ(mean[1], 4.0);
        EXPECT_THROW(sample_covariance({returns.data(), 1, 2}), Error);
    }

    // With fewer periods than assets the sample covariance is singular and
    // refused by the optimiser; the shrunk one is positive definite.
    TEST(Covariance, LedoitWolfIsUsableWithFewerPeriodsThanAssets)
    {
        const Index periods = 30;
        const Index assets = 60;
        const std::vector<Real> returns = factorReturns(periods, assets, 1);
        const std::vector<Real> lower(assets, 0.0);
        const std::vector<Real> upper(assets, 1.0);
        const Covariance sample = sample_covariance({returns.data(), periods, assets});
        EXPECT_THROW(minimum_variance(sample.view(), lower, upper), Error);

        const Covariance shrunk = ledoit_wolf_covariance({returns.data(), periods, assets});
        EXPECT_GT(shrunk.shrinkage, 0.0);
        EXPECT_LE(shrunk.shrinkage, 1.0);
        const std::vector<Real> w = minimum_variance(shrunk.view(), lower, upper);
        EXPECT_NEAR(sum(w), 1.0, 1e-12);
    }

    // The shrunk estimator is, on average, closer to the true covariance than
    // the sample one, and the shrinkage vanishes as the sample grows.
    TEST(Covariance, LedoitWolfIsCloserToTheTruthAndVanishesWithData)
    {
        const Index assets = 40;
        // The true covariance, from a very long sample of the same process.
        std::mt19937_64 rng(2);
        std::normal_distribution<Real> normal(0.0, 1.0);
        std::vector<Real> beta(assets);
        std::vector<Real> specific(assets);
        for (Index i = 0; i < assets; ++i)
        {
            beta[i] = 0.5 + static_cast<Real>(i) / static_cast<Real>(assets);
            specific[i] = 0.01 + 0.0005 * static_cast<Real>(i);
        }
        std::vector<Real> truth(assets * assets);
        for (Index i = 0; i < assets; ++i)
        {
            for (Index j = 0; j < assets; ++j)
            {
                truth[i * assets + j] = beta[i] * beta[j] * 1e-4 + (i == j ? specific[i] * specific[i] : 0.0);
            }
        }
        const auto draw = [&](Index periods)
        {
            std::vector<Real> returns(periods * assets);
            for (Index t = 0; t < periods; ++t)
            {
                const Real market = 0.01 * normal(rng);
                for (Index i = 0; i < assets; ++i)
                {
                    returns[t * assets + i] = beta[i] * market + specific[i] * normal(rng);
                }
            }
            return returns;
        };
        const auto distance = [&](const Covariance &c)
        {
            Real total = 0.0;
            for (Index k = 0; k < truth.size(); ++k)
            {
                total += (c.matrix[k] - truth[k]) * (c.matrix[k] - truth[k]);
            }
            return total;
        };
        Real sampleError = 0.0;
        Real shrunkError = 0.0;
        for (int repetition = 0; repetition < 50; ++repetition)
        {
            const std::vector<Real> returns = draw(50);
            sampleError += distance(sample_covariance({returns.data(), 50, assets}));
            shrunkError += distance(ledoit_wolf_covariance({returns.data(), 50, assets}));
        }
        EXPECT_LT(shrunkError, sampleError);

        const std::vector<Real> small = draw(60);
        const std::vector<Real> large = draw(20000);
        const Real fewPeriods = ledoit_wolf_covariance({small.data(), 60, assets}).shrinkage;
        const Real manyPeriods = ledoit_wolf_covariance({large.data(), 20000, assets}).shrinkage;
        EXPECT_GT(fewPeriods, 10.0 * manyPeriods);
        EXPECT_LT(manyPeriods, 0.01);
    }

    // ---- Minimum variance ----

    TEST(MinimumVariance, MatchesTheClosedFormWithoutActiveBounds)
    {
        // Two uncorrelated assets: weights inversely proportional to variance.
        const std::vector<Real> sigma = {0.04, 0.0,
                                         0.0, 0.01};
        const std::vector<Real> w = minimum_variance({sigma.data(), 2, 2}, std::vector<Real>{0.0, 0.0}, std::vector<Real>{1.0, 1.0});
        EXPECT_NEAR(w[0], 0.2, 1e-12);
        EXPECT_NEAR(w[1], 0.8, 1e-12);
        // With a cap of 60 % the second asset stops exactly at the cap.
        const std::vector<Real> capped = minimum_variance({sigma.data(), 2, 2}, std::vector<Real>{0.0, 0.0}, std::vector<Real>{0.6, 0.6});
        EXPECT_EQ(capped[1], 0.6);
        EXPECT_NEAR(capped[0], 0.4, 1e-15);
    }

    // Acceptance criteria of work package B4: the variance is at most the one
    // of any other admissible portfolio drawn at random, and no weight is
    // outside its bounds.
    TEST(MinimumVariance, BeatsEveryRandomAdmissiblePortfolio)
    {
        Random random(3);
        for (const Index assets : {Index{5}, Index{30}, Index{120}})
        {
            const std::vector<Real> returns = factorReturns(400, assets, 10 + assets);
            const Covariance covariance = sample_covariance({returns.data(), 400, assets});
            for (const Real cap : {1.0, 3.0 / static_cast<Real>(assets), 1.2 / static_cast<Real>(assets)})
            {
                const Real floor = cap < 1.0 ? 0.2 / static_cast<Real>(assets) : 0.0;
                const std::vector<Real> lower(assets, floor);
                const std::vector<Real> upper(assets, cap);
                const std::vector<Real> w = minimum_variance(covariance.view(), lower, upper);
                EXPECT_NEAR(sum(w), 1.0, 1e-12);
                for (const Real weight : w)
                {
                    // Exactly: a weight at a bound is equal to it.
                    ASSERT_GE(weight, floor);
                    ASSERT_LE(weight, cap);
                }
                const Real variance = portfolio_variance(covariance.view(), w);
                for (int trial = 0; trial < 300; ++trial)
                {
                    const std::vector<Real> other = randomAdmissible(assets, floor, cap, random);
                    ASSERT_LE(variance, portfolio_variance(covariance.view(), other) * (1.0 + 1e-12)) << assets << " assets, cap " << cap;
                }
            }
        }
    }

    // Finding 5: a cap that the number of assets cannot fill is an explicit
    // failure, not a result taken as it comes.
    TEST(MinimumVariance, RefusesAnInfeasibleProblem)
    {
        const std::vector<Real> returns = factorReturns(100, 4, 4);
        const Covariance covariance = sample_covariance({returns.data(), 100, 4});
        const std::vector<Real> zero(4, 0.0);
        // Four assets capped at 20 %: 80 % at most.
        try
        {
            minimum_variance(covariance.view(), zero, std::vector<Real>(4, 0.2));
            FAIL() << "an infeasible problem was solved";
        }
        catch (const Error &error)
        {
            EXPECT_NE(std::string(error.what()).find("infeasible"), std::string::npos);
        }
        EXPECT_THROW(minimum_variance(covariance.view(), std::vector<Real>(4, 0.3), std::vector<Real>(4, 1.0)), Error);
        EXPECT_THROW(minimum_variance(covariance.view(), std::vector<Real>(4, 0.5), std::vector<Real>(4, 0.4)), Error);
        EXPECT_THROW(minimum_variance(covariance.view(), zero, std::vector<Real>(3, 1.0)), Error);
        // Exactly feasible: one solution, the caps themselves.
        const std::vector<Real> w = minimum_variance(covariance.view(), zero, std::vector<Real>(4, 0.25));
        for (const Real weight : w)
        {
            EXPECT_EQ(weight, 0.25);
        }
    }

    // ---- Maximum Sharpe ----

    TEST(MaximumSharpe, MatchesTheTangencyPortfolioWithoutActiveBounds)
    {
        const Index assets = 6;
        const std::vector<Real> returns = factorReturns(2000, assets, 5);
        const Covariance covariance = sample_covariance({returns.data(), 2000, assets});
        const std::vector<Real> mu = {0.0010, 0.0012, 0.0008, 0.0011, 0.0009, 0.0013};
        // Bounds wide enough never to bind: weights proportional to the
        // inverse covariance times the expected returns.
        const std::vector<Real> w = maximum_sharpe(covariance.view(), mu, std::vector<Real>(assets, -5.0), std::vector<Real>(assets, 5.0));
        std::vector<Real> tangency(assets);
        {
            // Solve Sigma x = mu by the optimiser's own unconstrained
            // minimum of (1/2) x'Sx - mu'x is avoided: plain Gauss elimination.
            std::vector<Real> a(covariance.matrix);
            std::vector<Real> b(mu);
            for (Index k = 0; k < assets; ++k)
            {
                for (Index i = k + 1; i < assets; ++i)
                {
                    const Real factor = a[i * assets + k] / a[k * assets + k];
                    for (Index j = k; j < assets; ++j)
                    {
                        a[i * assets + j] -= factor * a[k * assets + j];
                    }
                    b[i] -= factor * b[k];
                }
            }
            for (Index k = assets; k-- > 0;)
            {
                Real value = b[k];
                for (Index j = k + 1; j < assets; ++j)
                {
                    value -= a[k * assets + j] * tangency[j];
                }
                tangency[k] = value / a[k * assets + k];
            }
            const Real total = sum(tangency);
            for (Real &x : tangency)
            {
                x /= total;
            }
        }
        for (Index i = 0; i < assets; ++i)
        {
            EXPECT_NEAR(w[i], tangency[i], 1e-7);
        }
    }

    TEST(MaximumSharpe, BeatsEveryRandomAdmissiblePortfolioWithinItsBounds)
    {
        Random random(6);
        const Index assets = 25;
        const std::vector<Real> returns = factorReturns(500, assets, 7);
        const Covariance covariance = sample_covariance({returns.data(), 500, assets});
        const std::vector<Real> mu = mean_returns({returns.data(), 500, assets});
        const auto sharpe = [&](const std::vector<Real> &w)
        {
            Real expected = 0.0;
            for (Index i = 0; i < assets; ++i)
            {
                expected += w[i] * mu[i];
            }
            return expected / std::sqrt(portfolio_variance(covariance.view(), w));
        };
        for (const Real cap : {1.0, 0.15, 0.05})
        {
            const std::vector<Real> lower(assets, 0.0);
            const std::vector<Real> upper(assets, cap);
            const std::vector<Real> w = maximum_sharpe(covariance.view(), mu, lower, upper);
            EXPECT_NEAR(sum(w), 1.0, 1e-12);
            for (const Real weight : w)
            {
                ASSERT_GE(weight, 0.0);
                ASSERT_LE(weight, cap);
            }
            const Real best = sharpe(w);
            for (int trial = 0; trial < 500; ++trial)
            {
                ASSERT_LE(sharpe(randomAdmissible(assets, 0.0, cap, random)), best + 1e-10) << "cap " << cap;
            }
            // A tighter cap cannot do better.
            EXPECT_LE(best, sharpe(maximum_sharpe(covariance.view(), mu, lower, std::vector<Real>(assets, 1.0))) + 1e-10);
        }
    }

    TEST(MaximumSharpe, RefusesWhenNoPortfolioHasAPositiveExpectedReturn)
    {
        const std::vector<Real> sigma = {0.04, 0.0,
                                         0.0, 0.01};
        const std::vector<Real> zero(2, 0.0);
        const std::vector<Real> one(2, 1.0);
        EXPECT_THROW(maximum_sharpe({sigma.data(), 2, 2}, std::vector<Real>{-0.01, -0.02}, zero, one), Error);
        // One asset with a positive expected return is enough.
        const std::vector<Real> w = maximum_sharpe({sigma.data(), 2, 2}, std::vector<Real>{-0.01, 0.02}, zero, one);
        EXPECT_NEAR(w[1], 1.0, 1e-9);
        EXPECT_NEAR(w[0], 0.0, 1e-9);
    }

    // ---- Risk parity ----

    // Acceptance criterion of work package B4: equal contributions to 1e-6.
    TEST(RiskParity, ContributionsAreEqual)
    {
        for (const Index assets : {Index{2}, Index{10}, Index{80}})
        {
            const std::vector<Real> returns = factorReturns(600, assets, 20 + assets);
            const Covariance covariance = sample_covariance({returns.data(), 600, assets});
            const std::vector<Real> w = risk_parity(covariance.view());
            EXPECT_NEAR(sum(w), 1.0, 1e-12);
            const std::vector<Real> contributions = risk_contributions(covariance.view(), w);
            EXPECT_NEAR(sum(contributions), 1.0, 1e-12);
            for (Index i = 0; i < assets; ++i)
            {
                EXPECT_GT(w[i], 0.0);
                EXPECT_NEAR(contributions[i], 1.0 / static_cast<Real>(assets), 1e-6);
            }
        }
    }

    TEST(RiskParity, IsInverseVolatilityForUncorrelatedAssets)
    {
        const std::vector<Real> sigma = {0.04, 0.0, 0.0,
                                         0.0, 0.01, 0.0,
                                         0.0, 0.0, 0.0025};
        const std::vector<Real> w = risk_parity({sigma.data(), 3, 3});
        // 1/0.2, 1/0.1, 1/0.05, normalised.
        EXPECT_NEAR(w[0], 5.0 / 35.0, 1e-10);
        EXPECT_NEAR(w[1], 10.0 / 35.0, 1e-10);
        EXPECT_NEAR(w[2], 20.0 / 35.0, 1e-10);
        const std::vector<Real> singular = {1.0, 1.0,
                                            1.0, 1.0};
        EXPECT_THROW(risk_parity({singular.data(), 2, 2}), Error);
    }

    // ---- The strategy in the engine ----

    namespace
    {

        AllocationConfig allocation(AllocationMethod method)
        {
            AllocationConfig c;
            c.method = method;
            c.lookback = 60;
            c.rebalance_every = 20;
            return c;
        }

        BacktestConfig backtest(const CostModel *costs = nullptr)
        {
            BacktestConfig c;
            c.initial_capital = 1e6;
            c.costs = costs;
            return c;
        }

    } // namespace

    TEST(AllocationStrategy, EveryMethodRunsAndRespectsItsBounds)
    {
        testing::SyntheticMarket m = testing::makeMarket(300, 12, 30);
        m.addSplit(3, 150, 2.0);
        for (const AllocationMethod method : {AllocationMethod::EqualWeight, AllocationMethod::MinimumVariance,
                                              AllocationMethod::MaximumSharpe, AllocationMethod::RiskParity})
        {
            AllocationConfig config = allocation(method);
            if (method != AllocationMethod::RiskParity)
            {
                config.max_weight = 0.25;
            }
            AllocationStrategy strategy(config);
            const BacktestResult r = run_backtest(m.data(), strategy, backtest());
            ASSERT_EQ(strategy.history().size(), 12u) << strategy.describe();
            for (const AllocationRecord &record : strategy.history())
            {
                EXPECT_EQ(record.eligible, 12u);
                EXPECT_NEAR(sum(record.weights), 1.0, 1e-12);
                for (const Real w : record.weights)
                {
                    EXPECT_GE(w, 0.0);
                    EXPECT_LE(w, config.max_weight);
                }
                EXPECT_TRUE(std::isfinite(record.in_sample_sharpe));
            }
            EXPECT_GT(r.journal.size(), 12u);
            EXPECT_FALSE(strategy.describe().empty());
        }
    }

    // The weights the engine holds after a rebalancing are the ones decided.
    TEST(AllocationStrategy, TheBookHoldsTheDecidedWeights)
    {
        const testing::SyntheticMarket m = testing::makeMarket(200, 8, 31);
        AllocationStrategy strategy(allocation(AllocationMethod::MinimumVariance));
        const BacktestResult r = run_backtest(m.data(), strategy, backtest());
        const AllocationRecord &first = strategy.history().front();
        EXPECT_EQ(first.date, m.dates[60]);
        // Decided on day 60, executed at the close of day 61.
        for (Index i = 0; i < m.assets; ++i)
        {
            const Real held = r.shares[m.at(61, i)] * m.close[m.at(61, i)] / r.value[61];
            EXPECT_NEAR(held, first.weights[i], 1e-10);
        }
    }

    // Finding 9: every rebalancing reconsiders every asset. An asset the
    // optimiser left out comes back when the data change.
    TEST(AllocationStrategy, AnAssetLeftOutCanComeBack)
    {
        testing::SyntheticMarket m = testing::makeMarket(260, 3, 32, {.dividends = false, .annualRate = 0.0});
        // At first asset 0 is asset 1 with three times the leverage: more
        // risk and no diversification, so minimum variance leaves it out.
        // Afterwards it is quiet and independent.
        std::mt19937_64 rng(1);
        std::normal_distribution<Real> normal(0.0, 1.0);
        Real price = 100.0;
        for (Index t = 0; t < m.days; ++t)
        {
            if (t > 0)
            {
                const Real other = m.close[m.at(t, 1)] / m.close[m.at(t - 1, 1)] - 1.0;
                price *= t < 130 ? 1.0 + 3.0 * other + 0.0005 * normal(rng) : 1.0 + 0.001 * normal(rng);
            }
            for (std::vector<Real> *table : {&m.open, &m.high, &m.low, &m.close})
            {
                (*table)[m.at(t, 0)] = price;
            }
        }
        AllocationConfig config = allocation(AllocationMethod::MinimumVariance);
        AllocationStrategy strategy(config);
        run_backtest(m.data(), strategy, backtest());
        const std::vector<AllocationRecord> &history = strategy.history();
        EXPECT_EQ(history.front().weights[0], 0.0);
        EXPECT_GT(history.back().weights[0], 0.5);
    }

    // Assets are held only once they have a full window of quotes, and are
    // sold when they stop quoting; members of the universe only.
    TEST(AllocationStrategy, HoldsOnlyWhatIsQuotedAndInTheUniverse)
    {
        testing::SyntheticMarket m = testing::makeMarket(200, 5, 33);
        for (Index t = 0; t < 50; ++t)
        {
            for (std::vector<Real> *table : {&m.open, &m.high, &m.low, &m.close})
            {
                (*table)[m.at(t, 4)] = std::numeric_limits<Real>::quiet_NaN();
            }
        }
        m.universe.assign(m.days * m.assets, 1);
        for (Index t = 150; t < m.days; ++t)
        {
            m.universe[m.at(t, 1)] = 0;
        }
        AllocationStrategy strategy(allocation(AllocationMethod::EqualWeight));
        run_backtest(m.data(), strategy, backtest());
        for (const AllocationRecord &record : strategy.history())
        {
            const Index day = static_cast<Index>(std::lower_bound(m.dates.begin(), m.dates.end(), record.date) - m.dates.begin());
            // Asset 4 has its first full window (61 closes) on day 110.
            EXPECT_EQ(record.weights[4] > 0.0, day >= 110) << "day " << day;
            EXPECT_EQ(record.weights[1] > 0.0, day < 150) << "day " << day;
            EXPECT_NEAR(sum(record.weights), 1.0, 1e-12);
        }
    }

    // Finding 5 again, through the engine: a cap the eligible assets cannot
    // fill stops the run.
    TEST(AllocationStrategy, AnInfeasibleCapStopsTheRun)
    {
        const testing::SyntheticMarket m = testing::makeMarket(150, 4, 34);
        AllocationConfig config = allocation(AllocationMethod::MinimumVariance);
        config.max_weight = 0.2;
        AllocationStrategy strategy(config);
        EXPECT_THROW(run_backtest(m.data(), strategy, backtest()), Error);

        config.method = AllocationMethod::RiskParity;
        EXPECT_THROW(AllocationStrategy{config}, Error);
        config = allocation(AllocationMethod::MinimumVariance);
        config.min_weight = -0.1;
        EXPECT_THROW(AllocationStrategy{config}, Error);
        config = allocation(AllocationMethod::MinimumVariance);
        config.lookback = 1;
        EXPECT_THROW(AllocationStrategy{config}, Error);
    }

    // With more assets than periods the sample covariance is refused, with
    // the remedy in the message; the shrunk one runs.
    TEST(AllocationStrategy, SampleCovarianceIsRefusedWhenSingular)
    {
        const testing::SyntheticMarket m = testing::makeMarket(120, 40, 35);
        AllocationConfig config = allocation(AllocationMethod::MinimumVariance);
        config.lookback = 30;
        config.covariance = CovarianceEstimator::Sample;
        AllocationStrategy sample(config);
        try
        {
            run_backtest(m.data(), sample, backtest());
            FAIL() << "a singular covariance was used";
        }
        catch (const Error &error)
        {
            EXPECT_NE(std::string(error.what()).find("shrunk estimator"), std::string::npos);
        }
        config.covariance = CovarianceEstimator::LedoitWolf;
        AllocationStrategy shrunk(config);
        EXPECT_NO_THROW(run_backtest(m.data(), shrunk, backtest()));
        EXPECT_GT(shrunk.history().front().shrinkage, 0.0);
    }

    TEST(AllocationStrategy, RebalancesOnTheFirstDayOfEachMonth)
    {
        const testing::SyntheticMarket m = testing::makeMarket(200, 4, 36);
        AllocationConfig config = allocation(AllocationMethod::EqualWeight);
        config.schedule = RebalanceSchedule::FirstDayOfMonth;
        AllocationStrategy strategy(config);
        run_backtest(m.data(), strategy, backtest());
        ASSERT_GE(strategy.history().size(), 5u);
        for (const AllocationRecord &record : strategy.history())
        {
            const Index day = static_cast<Index>(std::lower_bound(m.dates.begin(), m.dates.end(), record.date) - m.dates.begin());
            EXPECT_NE(format_date(m.dates[day]).substr(5, 2), format_date(m.dates[day - 1]).substr(5, 2));
        }
    }

    // Walk-forward with a growing window: each decision uses every return
    // before it, and still none after it.
    TEST(AllocationStrategy, ExpandingWindowUsesAllThePastAndOnlyThePast)
    {
        const testing::SyntheticMarket m = testing::makeMarket(200, 5, 39);
        AllocationConfig config = allocation(AllocationMethod::MinimumVariance);
        AllocationStrategy sliding(config);
        run_backtest(m.data(), sliding, backtest());
        config.expanding = true;
        AllocationStrategy growing(config);
        run_backtest(m.data(), growing, backtest());
        ASSERT_EQ(growing.history().size(), sliding.history().size());
        // On the first decision the two windows are nearly the same; later
        // they are not.
        EXPECT_NE(growing.history().back().weights, sliding.history().back().weights);
        EXPECT_NE(growing.describe().find("every daily return"), std::string::npos);

        // Changing the data after a decision day does not change that decision.
        testing::SyntheticMarket changed = m;
        for (Index t = 121; t < m.days; ++t)
        {
            for (std::vector<Real> *table : {&changed.open, &changed.high, &changed.low, &changed.close})
            {
                (*table)[changed.at(t, 2)] *= 1.5;
            }
        }
        AllocationStrategy again(config);
        run_backtest(changed.data(), again, backtest());
        for (Index k = 0; k < growing.history().size(); ++k)
        {
            if (growing.history()[k].date <= m.dates[120])
            {
                EXPECT_EQ(again.history()[k].weights, growing.history()[k].weights);
            }
        }
        EXPECT_NE(again.history().back().weights, growing.history().back().weights);
    }

    // Finding 12: on returns without any signal, the Sharpe ratio the
    // optimiser reports on its own window is far above what the same weights
    // earn afterwards. It measures overfitting, not skill.
    TEST(AllocationStrategy, InSampleSharpeOfTheOptimisedPortfolioIsBiasedUpwards)
    {
        // Twenty assets with the same expected return and no rate: any
        // difference between them over a window is noise.
        testing::SyntheticMarket m = testing::makeMarket(1500, 20, 37, {.dividends = false, .annualRate = 0.0});
        AllocationConfig config = allocation(AllocationMethod::MaximumSharpe);
        AllocationStrategy strategy(config);
        const BacktestResult r = run_backtest(m.data(), strategy, backtest());
        Real inSample = 0.0;
        for (const AllocationRecord &record : strategy.history())
        {
            inSample += record.in_sample_sharpe / static_cast<Real>(strategy.history().size());
        }
        const std::vector<Real> returns = simple_returns(std::span<const Real>(r.value).subspan(62));
        const Real realised = sharpe_ratio(returns) * std::sqrt(252.0);
        EXPECT_GT(inSample, 3.0);
        EXPECT_GT(inSample, realised + 2.0);
    }

    // Finding 13: the comparison points come with the strategy, on the same
    // assets, the same days and the same costs.
    TEST(ComparedRuns, EqualWeightAndBuyAndHoldAreAlwaysReturned)
    {
        const testing::SyntheticMarket m = testing::makeMarket(300, 10, 38);
        const ProportionalCost cost(0.001);
        AllocationConfig config = allocation(AllocationMethod::MinimumVariance);
        config.max_weight = 0.3;
        std::vector<double> progress;
        RunControl control;
        control.progress = [&](double fraction)
        { progress.push_back(fraction); };
        const ComparedRuns runs = run_with_comparisons(m.data(), config, backtest(&cost), control);

        ASSERT_EQ(runs.strategy.value.size(), m.days);
        ASSERT_EQ(runs.equal_weight.value.size(), m.days);
        ASSERT_EQ(runs.buy_and_hold.value.size(), m.days);
        EXPECT_FALSE(runs.description.empty());
        EXPECT_EQ(runs.allocations.size(), 12u);
        EXPECT_TRUE(std::is_sorted(progress.begin(), progress.end()));
        EXPECT_DOUBLE_EQ(progress.back(), 1.0);

        // Same execution days for the strategy and for equal weight.
        std::vector<Date> strategyDays;
        std::vector<Date> equalDays;
        for (const OrderRecord &order : runs.strategy.journal)
        {
            strategyDays.push_back(order.execution_date);
        }
        for (const OrderRecord &order : runs.equal_weight.journal)
        {
            equalDays.push_back(order.execution_date);
        }
        strategyDays.erase(std::unique(strategyDays.begin(), strategyDays.end()), strategyDays.end());
        equalDays.erase(std::unique(equalDays.begin(), equalDays.end()), equalDays.end());
        EXPECT_EQ(strategyDays, equalDays);

        // Equal weight: one tenth each after the first execution, with costs.
        for (Index i = 0; i < m.assets; ++i)
        {
            EXPECT_NEAR(runs.equal_weight.shares[m.at(61, i)] * m.close[m.at(61, i)] / runs.equal_weight.value[61], 0.1, 1e-10);
        }
        Real equalCosts = 0.0;
        for (const Real c : runs.equal_weight.costs)
        {
            equalCosts += c;
        }
        EXPECT_GT(equalCosts, 0.0);

        // Buy and hold: the strategy's first weights, bought once.
        for (const OrderRecord &order : runs.buy_and_hold.journal)
        {
            EXPECT_EQ(order.execution_date, m.dates[61]);
        }
        for (Index i = 0; i < m.assets; ++i)
        {
            EXPECT_NEAR(runs.buy_and_hold.shares[m.at(61, i)], runs.strategy.shares[m.at(61, i)], 1e-6 * (1.0 + runs.strategy.shares[m.at(61, i)]));
        }
        EXPECT_FALSE(runs.strategy.quality.issues.size() > 1000);
    }

} // namespace quantBacktest
