#include <gtest/gtest.h>

#include "quantBacktest/core/error.hpp"
#include "quantBacktest/inference/bootstrap.hpp"
#include "quantBacktest/inference/performance_report.hpp"
#include "quantBacktest/inference/sharpe_difference.hpp"
#include "quantBacktest/inference/sharpe_inference.hpp"
#include "quantBacktest/metrics/performance.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <random>
#include <set>
#include <vector>

namespace quantBacktest
{

    namespace
    {

        std::vector<Real> gaussian(Index count, Real mean, Real deviation, std::mt19937_64 &rng)
        {
            std::normal_distribution<Real> normal(mean, deviation);
            std::vector<Real> out(count);
            for (Real &x : out)
            {
                x = normal(rng);
            }
            return out;
        }

        PerformanceConfig reportConfig(Index replications, Real blockLength = 1.0, Index threads = 1)
        {
            PerformanceConfig config;
            config.bootstrap.replications = replications;
            config.bootstrap.mean_block_length = blockLength;
            config.bootstrap.seed = 7;
            config.bootstrap.threads = threads;
            return config;
        }

    } // namespace

    // ---- Section 8.1 ----

    TEST(SharpeInference, StandardErrorFormula)
    {
        // Gaussian returns: sqrt((1 + SR^2 / 2) / T).
        EXPECT_NEAR(sharpe_standard_error(0.1, 0.0, 3.0, 1000), std::sqrt((1.0 + 0.005) / 1000.0), 1e-15);
        // Negative skewness and fat tails widen it.
        EXPECT_GT(sharpe_standard_error(0.1, -1.0, 8.0, 1000), sharpe_standard_error(0.1, 0.0, 3.0, 1000));
        EXPECT_NEAR(sharpe_standard_error(0.1, -1.0, 8.0, 1000), std::sqrt((1.0 + 0.1 + 1.75 * 0.01) / 1000.0), 1e-15);
    }

    // Finding 11, with the figures of the companion: an annualised Sharpe
    // ratio of 0.8 measured over three years has a standard error of about
    // 0.58 and a 95 % interval of about [-0.3, 1.9]. It is not
    // distinguishable from zero, and the estimate says so.
    TEST(SharpeInference, ThreeYearsDoNotSeparateASharpeOfPointEightFromZero)
    {
        const Index observations = 3 * 252;
        const Real perPeriod = 0.8 / std::sqrt(252.0);
        const Real error = sharpe_standard_error(perPeriod, 0.0, 3.0, observations) * std::sqrt(252.0);
        EXPECT_NEAR(error, 0.58, 0.005);
        EXPECT_NEAR(0.8 - 1.96 * error, -0.3, 0.05);
        EXPECT_NEAR(0.8 + 1.96 * error, 1.9, 0.05);
    }

    // Acceptance criterion of work package B3: on simulated Gaussian returns
    // of known Sharpe ratio, the 95 % interval holds the true value in
    // 95 % +/- 2 % of the draws.
    TEST(SharpeInference, IntervalCoversTheTrueSharpeRatio)
    {
        std::mt19937_64 rng(2026);
        const Real deviation = 0.01;
        for (const Real annualSharpe : {0.0, 0.8, 2.0})
        {
            const Real mean = annualSharpe / std::sqrt(252.0) * deviation;
            const int draws = 4000;
            int covered = 0;
            for (int draw = 0; draw < draws; ++draw)
            {
                const std::vector<Real> excess = gaussian(756, mean, deviation, rng);
                const Estimate estimate = sharpe_estimate(excess, 252.0, 0.95);
                covered += estimate.lower <= annualSharpe && annualSharpe <= estimate.upper ? 1 : 0;
            }
            const Real coverage = static_cast<Real>(covered) / draws;
            EXPECT_NEAR(coverage, 0.95, 0.02) << "true annualised Sharpe ratio " << annualSharpe;
        }
    }

    // The bootstrap percentile interval, on the same criterion. It is
    // slightly narrow: 94.0 % measured over 4000 draws through the report.
    // This test runs 800 of them to stay short, so its own sampling error is
    // 0.8 %, hence the wider tolerance.
    TEST(Bootstrap, PercentileIntervalCoversTheTrueSharpeRatio)
    {
        std::mt19937_64 rng(11);
        const Real deviation = 0.01;
        const Real annualSharpe = 0.8;
        const Real mean = annualSharpe / std::sqrt(252.0) * deviation;
        const int draws = 800;
        int covered = 0;
        std::vector<Real> resample(504);
        for (int draw = 0; draw < draws; ++draw)
        {
            const std::vector<Real> excess = gaussian(504, mean, deviation, rng);
            const BootstrapStatistic statistic = [&](std::span<const Index> indices, std::span<Real> out)
            {
                for (Index k = 0; k < indices.size(); ++k)
                {
                    resample[k] = excess[indices[k]];
                }
                out[0] = sharpe_ratio(resample) * std::sqrt(252.0);
            };
            BootstrapConfig config;
            config.replications = 300;
            config.seed = static_cast<std::uint64_t>(draw);
            const BootstrapInterval interval = percentile_interval(stationary_bootstrap(excess.size(), 1, statistic, config), 0.95);
            covered += interval.lower <= annualSharpe && annualSharpe <= interval.upper ? 1 : 0;
        }
        EXPECT_NEAR(static_cast<Real>(covered) / draws, 0.95, 0.03);
    }

    TEST(SharpeInference, EstimateIsAnnualisedAndSaysHow)
    {
        std::mt19937_64 rng(3);
        const std::vector<Real> excess = gaussian(1000, 0.0005, 0.01, rng);
        const Estimate estimate = sharpe_estimate(excess, 252.0, 0.95);
        EXPECT_DOUBLE_EQ(estimate.value, sharpe_ratio(excess) * std::sqrt(252.0));
        EXPECT_NEAR(estimate.upper - estimate.value, 1.959964 * estimate.standard_error, 1e-6);
        EXPECT_DOUBLE_EQ(estimate.confidence, 0.95);
        EXPECT_NE(estimate.method.find("Lo (2002)"), std::string::npos);
        EXPECT_NE(estimate.convention.find("arithmetic"), std::string::npos);
        EXPECT_THROW(sharpe_estimate(excess, 252.0, 1.0), Error);
        EXPECT_THROW(sharpe_estimate(std::vector<Real>(10, 0.001), 252.0, 0.95), Error);
    }

    // ---- Section 8.2 ----

    TEST(SharpeInference, ProbabilisticSharpeRatio)
    {
        // At the threshold itself the probability is one half.
        EXPECT_NEAR(probabilistic_sharpe_ratio(0.1, 0.1, -0.5, 5.0, 500), 0.5, 1e-15);
        // By hand, Gaussian: Phi(0.05 sqrt(999) / sqrt(1 + 0.00125)).
        EXPECT_NEAR(probabilistic_sharpe_ratio(0.05, 0.0, 0.0, 3.0, 1000),
                    normal_cdf(0.05 * std::sqrt(999.0) / std::sqrt(1.00125)), 1e-15);
        // More history, more certainty; negative skewness, less.
        EXPECT_GT(probabilistic_sharpe_ratio(0.05, 0.0, 0.0, 3.0, 2000), probabilistic_sharpe_ratio(0.05, 0.0, 0.0, 3.0, 1000));
        EXPECT_LT(probabilistic_sharpe_ratio(0.05, 0.0, -2.0, 10.0, 1000), probabilistic_sharpe_ratio(0.05, 0.0, 0.0, 3.0, 1000));
    }

    // The minimum track record length is the history at which the
    // probabilistic Sharpe ratio reaches the confidence level.
    TEST(SharpeInference, MinimumTrackRecordLengthInvertsThePsr)
    {
        for (const Real skew : {0.0, -1.5})
        {
            const Real kurt = skew == 0.0 ? 3.0 : 9.0;
            const Real length = minimum_track_record_length(0.06, 0.01, skew, kurt, 0.95);
            const Index rounded = static_cast<Index>(std::ceil(length));
            EXPECT_GE(probabilistic_sharpe_ratio(0.06, 0.01, skew, kurt, rounded), 0.95);
            EXPECT_LT(probabilistic_sharpe_ratio(0.06, 0.01, skew, kurt, rounded - 2), 0.95);
        }
        EXPECT_GT(minimum_track_record_length(0.06, 0.0, -1.5, 9.0, 0.95), minimum_track_record_length(0.06, 0.0, 0.0, 3.0, 0.95));
        EXPECT_TRUE(std::isinf(minimum_track_record_length(0.01, 0.02, 0.0, 3.0, 0.95)));
    }

    // ---- Section 8.3 ----

    TEST(Bootstrap, IndicesComeInBlocksOfTheExpectedLength)
    {
        Random random(1);
        const Index observations = 1000;
        std::vector<Index> indices(200000);
        stationary_bootstrap_indices(observations, 10.0, random, indices);
        Index blocks = 1;
        std::set<Index> seen;
        for (Index k = 0; k < indices.size(); ++k)
        {
            ASSERT_LT(indices[k], observations);
            seen.insert(indices[k]);
            if (k > 0 && indices[k] != (indices[k - 1] + 1) % observations)
            {
                ++blocks;
            }
        }
        EXPECT_NEAR(static_cast<Real>(indices.size()) / static_cast<Real>(blocks), 10.0, 0.3);
        EXPECT_EQ(seen.size(), observations);

        // A block length of 1 is the ordinary bootstrap: no run survives.
        stationary_bootstrap_indices(observations, 1.0, random, indices);
        Index followers = 0;
        for (Index k = 1; k < indices.size(); ++k)
        {
            followers += indices[k] == (indices[k - 1] + 1) % observations ? 1 : 0;
        }
        EXPECT_LT(followers, indices.size() / 200);
        EXPECT_THROW(stationary_bootstrap_indices(observations, 0.5, random, indices), Error);
        EXPECT_THROW(stationary_bootstrap_indices(0, 2.0, random, indices), Error);
    }

    // Determinism: same seed, same table to the bit, whatever the number of
    // threads; another seed, another table.
    TEST(Bootstrap, DoesNotDependOnTheNumberOfThreads)
    {
        std::mt19937_64 rng(5);
        const std::vector<Real> data = gaussian(300, 0.0, 1.0, rng);
        const BootstrapStatistic statistic = [&](std::span<const Index> indices, std::span<Real> out)
        {
            Real sum = 0.0;
            Real largest = -std::numeric_limits<Real>::infinity();
            for (const Index k : indices)
            {
                sum += data[k];
                largest = std::max(largest, data[k]);
            }
            out[0] = sum / static_cast<Real>(indices.size());
            out[1] = largest;
        };
        BootstrapConfig config;
        config.replications = 500;
        config.mean_block_length = 5.0;
        config.seed = 42;
        const std::vector<Real> one = stationary_bootstrap(data.size(), 2, statistic, config);
        for (const Index threads : {Index{2}, Index{3}, Index{8}, Index{1000}})
        {
            config.threads = threads;
            EXPECT_EQ(stationary_bootstrap(data.size(), 2, statistic, config), one) << threads << " threads";
        }
        config.seed = 43;
        EXPECT_NE(stationary_bootstrap(data.size(), 2, statistic, config), one);
        config.replications = 0;
        EXPECT_THROW(stationary_bootstrap(data.size(), 2, statistic, config), Error);
    }

    // The bootstrap standard error of a mean is the textbook one for
    // independent data, and larger for positively autocorrelated data when
    // the blocks are long enough to keep the dependence.
    TEST(Bootstrap, StandardErrorOfAMean)
    {
        std::mt19937_64 rng(6);
        const Index observations = 4000;
        const std::vector<Real> noise = gaussian(observations, 0.0, 1.0, rng);
        std::vector<Real> persistent(observations);
        persistent[0] = noise[0];
        for (Index t = 1; t < observations; ++t)
        {
            persistent[t] = 0.6 * persistent[t - 1] + noise[t];
        }
        const auto meanError = [&](const std::vector<Real> &series, Real blockLength)
        {
            const BootstrapStatistic statistic = [&](std::span<const Index> indices, std::span<Real> out)
            {
                Real sum = 0.0;
                for (const Index k : indices)
                {
                    sum += series[k];
                }
                out[0] = sum / static_cast<Real>(indices.size());
            };
            BootstrapConfig config;
            config.replications = 2000;
            config.mean_block_length = blockLength;
            config.seed = 9;
            config.threads = 4;
            return percentile_interval(stationary_bootstrap(series.size(), 1, statistic, config), 0.95).standard_error;
        };
        const Real textbook = 1.0 / std::sqrt(static_cast<Real>(observations));
        EXPECT_NEAR(meanError(noise, 1.0), textbook, 0.1 * textbook);
        // AR(1) with coefficient 0.6: the long-run standard error of the mean
        // is 1 / (1 - 0.6) times the one of its innovations.
        EXPECT_NEAR(meanError(persistent, 50.0), 2.5 * textbook, 0.4 * textbook);
        // The ordinary bootstrap misses it.
        EXPECT_LT(meanError(persistent, 1.0), 1.6 * textbook);
    }

    TEST(Bootstrap, PercentileInterval)
    {
        std::vector<Real> replications;
        for (int k = 0; k <= 100; ++k)
        {
            replications.push_back(static_cast<Real>(100 - k));
        }
        const BootstrapInterval interval = percentile_interval(replications, 0.90);
        EXPECT_DOUBLE_EQ(interval.lower, 5.0);
        EXPECT_DOUBLE_EQ(interval.upper, 95.0);
        EXPECT_NEAR(interval.standard_error, std::sqrt(101.0 * 102.0 / 12.0), 1e-9);
        replications[3] = std::numeric_limits<Real>::quiet_NaN();
        EXPECT_TRUE(std::isnan(percentile_interval(replications, 0.90).lower));
        EXPECT_THROW(percentile_interval(std::vector<Real>{1.0}, 0.9), Error);
    }

    TEST(Bootstrap, CancelsAndPropagatesErrors)
    {
        BootstrapConfig config;
        config.replications = 2000;
        config.threads = 4;
        const BootstrapStatistic slow = [](std::span<const Index> indices, std::span<Real> out)
        { out[0] = static_cast<Real>(indices[0]); };

        std::atomic<bool> cancel{false};
        std::vector<double> progress;
        RunControl control;
        control.cancel = &cancel;
        control.progress = [&](double fraction)
        {
            progress.push_back(fraction);
            cancel = fraction > 0.2;
        };
        EXPECT_THROW(stationary_bootstrap(100, 1, slow, config, control), Cancelled);
        EXPECT_TRUE(std::is_sorted(progress.begin(), progress.end()));

        const BootstrapStatistic failing = [](std::span<const Index>, std::span<Real>)
        { throw Error("the statistic failed"); };
        EXPECT_THROW(stationary_bootstrap(100, 1, failing, config), Error);
    }

    // ---- Section 8.4 ----

    namespace
    {

        // Two strategies with the given per-period means, unit-free volatility
        // of 1 % and correlation rho.
        void correlatedPair(Index count, Real meanFirst, Real meanSecond, Real rho, std::mt19937_64 &rng, std::vector<Real> &first, std::vector<Real> &second)
        {
            std::normal_distribution<Real> normal(0.0, 1.0);
            first.resize(count);
            second.resize(count);
            for (Index t = 0; t < count; ++t)
            {
                const Real common = normal(rng);
                const Real own = normal(rng);
                first[t] = meanFirst + 0.01 * common;
                second[t] = meanSecond + 0.01 * (rho * common + std::sqrt(1.0 - rho * rho) * own);
            }
        }

        BootstrapConfig noBootstrap()
        {
            return {};
        }

    } // namespace

    TEST(SharpeDifference, FormulaByHand)
    {
        std::mt19937_64 rng(21);
        std::vector<Real> a;
        std::vector<Real> b;
        correlatedPair(800, 0.0008, 0.0003, 0.6, rng, a, b);
        const SharpeDifference d = sharpe_difference(a, b, 252.0, 0.95, noBootstrap());
        const Real s1 = sharpe_ratio(a);
        const Real s2 = sharpe_ratio(b);
        const Real rho = d.correlation;
        EXPECT_NEAR(rho, 0.6, 0.05);
        const Real variance = (2.0 - 2.0 * rho + 0.5 * (s1 * s1 + s2 * s2 - 2.0 * s1 * s2 * rho * rho)) / 800.0;
        EXPECT_NEAR(d.analytical.value, (s1 - s2) * std::sqrt(252.0), 1e-12);
        EXPECT_NEAR(d.analytical.standard_error, std::sqrt(variance * 252.0), 1e-12);
        EXPECT_NEAR(d.z_statistic, (s1 - s2) / std::sqrt(variance), 1e-9);
        EXPECT_NEAR(d.p_value, 2.0 * normal_cdf(-std::abs(d.z_statistic)), 1e-12);
        EXPECT_FALSE(d.bootstrap.has_value());
        EXPECT_NE(d.analytical.method.find("Memmel"), std::string::npos);

        // A series against itself: no difference, nothing to reject.
        const SharpeDifference same = sharpe_difference(a, a, 252.0, 0.95, noBootstrap());
        EXPECT_NEAR(same.analytical.value, 0.0, 1e-12);
        EXPECT_NEAR(same.p_value, 1.0, 1e-6);

        EXPECT_THROW(sharpe_difference(a, std::vector<Real>(10, 0.0), 252.0, 0.95, noBootstrap()), Error);
        EXPECT_THROW(sharpe_difference(a, std::vector<Real>(800, 0.0), 252.0, 0.95, noBootstrap()), Error);
    }

    // Two Sharpe ratios on the same period are correlated: the more the two
    // strategies move together, the smaller a difference has to be to count.
    // Comparing the two intervals one by one would miss it.
    TEST(SharpeDifference, CorrelationSharpensTheTest)
    {
        std::mt19937_64 rng(22);
        std::vector<Real> a;
        std::vector<Real> b;
        correlatedPair(1500, 0.0006, 0.0006, 0.0, rng, a, b);
        const Real loose = sharpe_difference(a, b, 252.0, 0.95, noBootstrap()).analytical.standard_error;
        correlatedPair(1500, 0.0006, 0.0006, 0.95, rng, a, b);
        const Real tight = sharpe_difference(a, b, 252.0, 0.95, noBootstrap()).analytical.standard_error;
        EXPECT_LT(tight, 0.3 * loose);
    }

    // Section 16: the empirical size of the test matches its level when the
    // two Sharpe ratios are equal.
    TEST(SharpeDifference, HasItsNominalSizeWhenTheSharpeRatiosAreEqual)
    {
        std::mt19937_64 rng(23);
        std::vector<Real> a;
        std::vector<Real> b;
        const int draws = 4000;
        int rejected = 0;
        for (int draw = 0; draw < draws; ++draw)
        {
            correlatedPair(750, 0.0005, 0.0005, 0.7, rng, a, b);
            rejected += sharpe_difference(a, b, 252.0, 0.95, noBootstrap()).p_value < 0.05 ? 1 : 0;
        }
        EXPECT_NEAR(static_cast<Real>(rejected) / draws, 0.05, 0.02);
    }

    TEST(SharpeDifference, BootstrapHasItsNominalSizeAndSeesARealDifference)
    {
        std::mt19937_64 rng(24);
        std::vector<Real> a;
        std::vector<Real> b;
        BootstrapConfig config;
        config.replications = 300;
        config.mean_block_length = 1.0;
        const int draws = 600;
        int rejected = 0;
        for (int draw = 0; draw < draws; ++draw)
        {
            correlatedPair(500, 0.0005, 0.0005, 0.7, rng, a, b);
            config.seed = static_cast<std::uint64_t>(draw);
            const SharpeDifference d = sharpe_difference(a, b, 252.0, 0.95, config);
            ASSERT_TRUE(d.bootstrap.has_value());
            rejected += *d.bootstrap_p_value < 0.05 ? 1 : 0;
            // The interval and the p-value tell the same story, up to the
            // interpolation between two resampled values at the boundary.
            const bool excludesZero = d.bootstrap->lower > 0.0 || d.bootstrap->upper < 0.0;
            if (excludesZero != (*d.bootstrap_p_value < 0.05))
            {
                EXPECT_NEAR(*d.bootstrap_p_value, 0.05, 0.01) << "draw " << draw;
            }
        }
        // 600 draws: a sampling error of 0.9 %.
        EXPECT_NEAR(static_cast<Real>(rejected) / draws, 0.05, 0.03);

        // An annualised difference of about 2.4 between highly correlated
        // strategies, over six years: seen every time.
        correlatedPair(1500, 0.0020, 0.0005, 0.9, rng, a, b);
        config.seed = 1;
        const SharpeDifference d = sharpe_difference(a, b, 252.0, 0.95, config);
        EXPECT_LT(d.p_value, 1e-6);
        EXPECT_LT(*d.bootstrap_p_value, 0.01);
        EXPECT_GT(d.bootstrap->lower, 0.0);
        EXPECT_NEAR(d.bootstrap->standard_error, d.analytical.standard_error, 0.25 * d.analytical.standard_error);
    }

    // ---- The report ----

    // Finding 11: no figure without its uncertainty and its convention.
    TEST(PerformanceReport, EveryMetricHasAnIntervalAMethodAndAConvention)
    {
        std::mt19937_64 rng(12);
        const std::vector<Real> returns = gaussian(1000, 0.0006, 0.011, rng);
        const std::vector<Real> benchmark = gaussian(1000, 0.0004, 0.010, rng);
        const std::vector<Real> cash(1000, 0.0001);
        const PerformanceReport report = performance_report(returns, cash, benchmark, reportConfig(500, 5.0, 4));

        EXPECT_EQ(report.observations, 1000u);
        EXPECT_NEAR(report.years, 1000.0 / 252.0, 1e-12);
        EXPECT_TRUE(report.omitted.empty());
        EXPECT_EQ(report.metrics.size(), 14u);
        for (const NamedEstimate &metric : report.metrics)
        {
            const Estimate &e = metric.estimate;
            EXPECT_TRUE(std::isfinite(e.value)) << metric.name;
            EXPECT_GT(e.standard_error, 0.0) << metric.name;
            EXPECT_LT(e.lower, e.upper) << metric.name;
            EXPECT_DOUBLE_EQ(e.confidence, 0.95) << metric.name;
            EXPECT_FALSE(e.method.empty()) << metric.name;
            EXPECT_FALSE(e.convention.empty()) << metric.name;
        }
        // The values are those of the metric functions.
        const std::vector<Real> excess = excess_returns(returns, cash);
        EXPECT_DOUBLE_EQ(report.find("sharpe_ratio")->value, sharpe_ratio(excess) * std::sqrt(252.0));
        EXPECT_DOUBLE_EQ(report.find("sharpe_ratio_bootstrap")->value, report.find("sharpe_ratio")->value);
        EXPECT_DOUBLE_EQ(report.find("annualized_return")->value, cagr(returns, 252.0));
        EXPECT_DOUBLE_EQ(report.find("max_drawdown")->value, max_drawdown(cumulative_values(returns)));
        EXPECT_DOUBLE_EQ(report.find("value_at_risk")->value, historical_var(returns, 0.05));
        EXPECT_DOUBLE_EQ(report.find("tracking_error")->value, tracking_error(returns, benchmark, 252.0));
        EXPECT_LT(report.find("max_drawdown")->upper, 0.0);
        // The two intervals of the Sharpe ratio agree on independent returns.
        EXPECT_NEAR(report.find("sharpe_ratio_bootstrap")->standard_error, report.find("sharpe_ratio")->standard_error,
                    0.15 * report.find("sharpe_ratio")->standard_error);
        // The t statistic is the Sharpe ratio over its standard error.
        EXPECT_NEAR(report.t_statistic, report.find("sharpe_ratio")->value / report.find("sharpe_ratio")->standard_error, 1e-12);
        EXPECT_GT(report.probabilistic_sharpe_ratio, 0.5);
        EXPECT_GT(report.minimum_track_record_years, 0.0);
        EXPECT_EQ(report.find("no_such_metric"), nullptr);
    }

    // Finding 3: the report takes the rate of each period, and a rate that
    // moved through the history does not give the Sharpe ratio of its last
    // value.
    TEST(PerformanceReport, UsesTheRiskFreeRateOfEachPeriod)
    {
        std::mt19937_64 rng(13);
        const std::vector<Real> returns = gaussian(1008, 0.0004, 0.008, rng);
        // 5 % a year in the first half, 0 % in the second.
        std::vector<Real> cash(1008, 0.0);
        std::fill(cash.begin(), cash.begin() + 504, 0.05 / 252.0);
        const std::vector<Real> lastRate(1008, cash.back());
        const PerformanceConfig config = reportConfig(0);
        const Real sharpe = performance_report(returns, cash, {}, config).find("sharpe_ratio")->value;
        const Real withLastRate = performance_report(returns, lastRate, {}, config).find("sharpe_ratio")->value;
        EXPECT_LT(sharpe, withLastRate - 0.1);
        // A single number is not accepted in place of the series.
        EXPECT_THROW(performance_report(returns, std::vector<Real>{0.0001}, {}, config), Error);
    }

    // A metric that cannot be bracketed is not returned, and the report says
    // why.
    TEST(PerformanceReport, OmitsWhatItCannotBracket)
    {
        std::mt19937_64 rng(14);
        const std::vector<Real> returns = gaussian(500, 0.0005, 0.01, rng);
        const std::vector<Real> cash(500, 0.0);

        // Without the bootstrap, only the analytical Sharpe ratio is left.
        const PerformanceReport analytic = performance_report(returns, cash, {}, reportConfig(0));
        ASSERT_EQ(analytic.metrics.size(), 1u);
        EXPECT_EQ(analytic.metrics[0].name, "sharpe_ratio");
        EXPECT_EQ(analytic.omitted.size(), 10u);
        EXPECT_EQ(analytic.find("max_drawdown"), nullptr);

        // Cash alone: a Sharpe ratio of zero, no shape, no Sortino, no Calmar.
        const std::vector<Real> flat(500, 0.0001);
        const std::vector<Real> flatCash(500, 0.0001);
        const PerformanceReport cashOnly = performance_report(flat, flatCash, {}, reportConfig(200));
        ASSERT_NE(cashOnly.find("sharpe_ratio"), nullptr);
        EXPECT_EQ(cashOnly.find("sharpe_ratio")->value, 0.0);
        EXPECT_EQ(cashOnly.find("max_drawdown")->value, 0.0);
        for (const char *name : {"sortino_ratio", "calmar_ratio", "skewness", "kurtosis"})
        {
            EXPECT_EQ(cashOnly.find(name), nullptr) << name;
        }
        bool explained = false;
        for (const OmittedMetric &metric : cashOnly.omitted)
        {
            explained |= metric.name == "sortino_ratio" && !metric.reason.empty();
        }
        EXPECT_TRUE(explained);

        EXPECT_THROW(performance_report(std::vector<Real>{0.1, 0.2}, std::vector<Real>{0.0, 0.0}, {}, reportConfig(0)), Error);
        EXPECT_THROW(performance_report(returns, cash, std::vector<Real>{0.1}, reportConfig(0)), Error);
    }

    TEST(PerformanceReport, SameSeedSameReport)
    {
        std::mt19937_64 rng(15);
        const std::vector<Real> returns = gaussian(600, 0.0005, 0.01, rng);
        const std::vector<Real> cash(600, 0.0001);
        const PerformanceReport a = performance_report(returns, cash, {}, reportConfig(300, 10.0, 1));
        const PerformanceReport b = performance_report(returns, cash, {}, reportConfig(300, 10.0, 6));
        ASSERT_EQ(a.metrics.size(), b.metrics.size());
        for (Index k = 0; k < a.metrics.size(); ++k)
        {
            EXPECT_EQ(a.metrics[k].estimate.lower, b.metrics[k].estimate.lower) << a.metrics[k].name;
            EXPECT_EQ(a.metrics[k].estimate.upper, b.metrics[k].estimate.upper) << a.metrics[k].name;
        }
    }

} // namespace quantBacktest
