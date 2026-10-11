#include <gtest/gtest.h>

#include "quantBacktest/core/error.hpp"
#include "quantBacktest/core/estimate.hpp"
#include "quantBacktest/data/returns.hpp"
#include "quantBacktest/engine/backtest.hpp"
#include "quantBacktest/metrics/performance.hpp"

#include "support/synthetic_market.hpp"

#include <cmath>
#include <limits>
#include <random>
#include <vector>

namespace quantBacktest
{

    namespace
    {

        constexpr Real NaN = std::numeric_limits<Real>::quiet_NaN();

        std::vector<Real> gaussian(Index count, Real mean, Real deviation, std::uint64_t seed)
        {
            std::mt19937_64 rng(seed);
            std::normal_distribution<Real> normal(mean, deviation);
            std::vector<Real> out(count);
            for (Real &x : out)
            {
                x = normal(rng);
            }
            return out;
        }

    } // namespace

    TEST(Normal, DistributionAndQuantile)
    {
        EXPECT_NEAR(normal_cdf(0.0), 0.5, 1e-15);
        EXPECT_NEAR(normal_cdf(1.959963984540054), 0.975, 1e-12);
        EXPECT_NEAR(normal_quantile(0.975), 1.959963984540054, 1e-10);
        EXPECT_NEAR(normal_quantile(0.5), 0.0, 1e-12);
        EXPECT_NEAR(normal_quantile(0.01), -2.3263478740408408, 1e-10);
        EXPECT_THROW(normal_quantile(0.0), Error);
        EXPECT_THROW(normal_quantile(1.0), Error);
    }

    TEST(Metrics, ReturnsAndValuesAreInverse)
    {
        const std::vector<Real> values = {100.0, 110.0, 99.0, 99.0};
        const std::vector<Real> returns = simple_returns(values);
        ASSERT_EQ(returns.size(), 3u);
        EXPECT_NEAR(returns[0], 0.10, 1e-15);
        EXPECT_NEAR(returns[1], -0.10, 1e-15);
        EXPECT_DOUBLE_EQ(returns[2], 0.0);
        const std::vector<Real> rebuilt = cumulative_values(returns);
        ASSERT_EQ(rebuilt.size(), 4u);
        for (Index t = 0; t < 4; ++t)
        {
            EXPECT_NEAR(rebuilt[t] * 100.0, values[t], 1e-12);
        }
        EXPECT_THROW(simple_returns(std::vector<Real>{1.0}), Error);
        EXPECT_THROW(simple_returns(std::vector<Real>{1.0, 0.0, 1.0}), Error);
        EXPECT_THROW(simple_returns(std::vector<Real>{1.0, NaN}), Error);
    }

    TEST(Metrics, MomentsByHand)
    {
        const std::vector<Real> x = {1.0, 2.0, 3.0, 4.0, 10.0};
        EXPECT_DOUBLE_EQ(mean(x), 4.0);
        EXPECT_DOUBLE_EQ(standard_deviation(x), std::sqrt(50.0 / 4.0));
        // Central moments with the n denominator: m2 = 10, m3 = 36, m4 = 278.8.
        EXPECT_NEAR(skewness(x), 36.0 / std::pow(10.0, 1.5), 1e-12);
        EXPECT_NEAR(kurtosis(x), 278.8 / 100.0, 1e-12);
        const std::vector<Real> flat(10, 0.01);
        EXPECT_TRUE(std::isnan(skewness(flat)));
        EXPECT_TRUE(std::isnan(kurtosis(flat)));
    }

    TEST(Metrics, GaussianShape)
    {
        const std::vector<Real> x = gaussian(200000, 0.0, 0.01, 1);
        EXPECT_NEAR(skewness(x), 0.0, 0.03);
        EXPECT_NEAR(kurtosis(x), 3.0, 0.06);
    }

    TEST(Metrics, CagrIsGeometric)
    {
        // +10 % then -10 % loses 1 %. With two periods a year that is the
        // year's return; with one period a year it is spread over two years.
        const std::vector<Real> returns = {0.10, -0.10};
        EXPECT_NEAR(cagr(returns, 2.0), -0.01, 1e-12);
        EXPECT_NEAR(cagr(returns, 1.0), std::sqrt(0.99) - 1.0, 1e-12);
        EXPECT_THROW(cagr(std::vector<Real>{0.1, -1.0}, 252.0), Error);
    }

    // Finding 10: the Sharpe ratio is computed on arithmetic returns, not on
    // logarithmic ones.
    TEST(Metrics, SharpeUsesArithmeticReturns)
    {
        const std::vector<Real> excess = gaussian(2000, 0.001, 0.03, 2);
        const Real sharpe = sharpe_ratio(excess);
        EXPECT_DOUBLE_EQ(sharpe, mean(excess) / standard_deviation(excess));

        std::vector<Real> logarithmic(excess.size());
        for (Index t = 0; t < excess.size(); ++t)
        {
            logarithmic[t] = std::log1p(excess[t]);
        }
        const Real onLogs = mean(logarithmic) / standard_deviation(logarithmic);
        // Lower by about half the volatility: systematic, not rounding.
        EXPECT_LT(onLogs, sharpe - 0.01);
        EXPECT_DOUBLE_EQ(annualized_volatility(excess, 252.0), standard_deviation(excess) * std::sqrt(252.0));
    }

    // Finding 3: the excess return uses the rate of each period.
    TEST(Metrics, ExcessReturnsUseTheRateOfEachPeriod)
    {
        const std::vector<Real> returns = {0.010, 0.010, 0.010, 0.010};
        const std::vector<Real> rates = {0.0002, 0.0002, 0.0001, 0.0000};
        const std::vector<Real> excess = excess_returns(returns, rates);
        EXPECT_DOUBLE_EQ(excess[0], 0.0098);
        EXPECT_DOUBLE_EQ(excess[3], 0.0100);
        // Applying the last known rate to the whole past gives another mean.
        const std::vector<Real> lastRate(4, rates.back());
        EXPECT_NE(mean(excess), mean(excess_returns(returns, lastRate)));
        // One rate for the whole history is not an input.
        EXPECT_THROW(excess_returns(returns, std::vector<Real>{0.0001}), Error);
    }

    // Section 16: the risk-free asset alone has a Sharpe ratio of zero, not
    // infinite, and no drawdown. The series comes out of the engine.
    TEST(Metrics, CashAloneHasAZeroSharpeAndNoDrawdown)
    {
        const testing::SyntheticMarket m = testing::makeMarket(300, 2, 5);
        testing::Idle strategy;
        BacktestConfig config;
        config.initial_capital = 1e6;
        const BacktestResult r = run_backtest(m.data(), strategy, config);
        const std::vector<Real> returns = simple_returns(r.value);
        const std::vector<Real> rates(m.cashRate.begin() + 1, m.cashRate.end());
        const std::vector<Real> excess = excess_returns(returns, rates);

        EXPECT_EQ(sharpe_ratio(excess), 0.0);
        EXPECT_EQ(max_drawdown(r.value), 0.0);
        EXPECT_EQ(longest_underwater(r.value), 0u);
        EXPECT_TRUE(std::isnan(sortino_ratio(excess)));
        EXPECT_TRUE(std::isnan(calmar_ratio(returns, 252.0)));

        // A mean without volatility has no Sharpe ratio.
        EXPECT_TRUE(std::isnan(sharpe_ratio(std::vector<Real>(10, 0.001))));
    }

    // Section 16: doubling the capital changes no ratio.
    TEST(Metrics, RatiosDoNotDependOnTheCapital)
    {
        const testing::SyntheticMarket m = testing::makeMarket(300, 4, 6);
        const auto run = [&](Real capital)
        {
            testing::Momentum strategy(20, 5);
            BacktestConfig config;
            config.initial_capital = capital;
            return run_backtest(m.data(), strategy, config).value;
        };
        const std::vector<Real> small = simple_returns(run(1e6));
        const std::vector<Real> large = simple_returns(run(2e6));
        EXPECT_NEAR(sharpe_ratio(large), sharpe_ratio(small), 1e-9);
        EXPECT_NEAR(cagr(large, 252.0), cagr(small, 252.0), 1e-9);
        EXPECT_NEAR(max_drawdown(cumulative_values(large)), max_drawdown(cumulative_values(small)), 1e-9);
    }

    // Section 16: leverage financed at the risk-free rate does not change the
    // Sharpe ratio. Excess returns scale with the leverage.
    TEST(Metrics, LeverageAtTheRiskFreeRateKeepsTheSharpeRatio)
    {
        const std::vector<Real> returns = gaussian(1500, 0.0006, 0.012, 4);
        const std::vector<Real> rates(returns.size(), 0.0001);
        std::vector<Real> levered(returns.size());
        for (Index t = 0; t < returns.size(); ++t)
        {
            levered[t] = 2.0 * returns[t] - rates[t];
        }
        EXPECT_NEAR(sharpe_ratio(excess_returns(levered, rates)), sharpe_ratio(excess_returns(returns, rates)), 1e-12);
    }

    TEST(Metrics, SortinoByHand)
    {
        const std::vector<Real> excess = {0.02, -0.01, 0.03, -0.02};
        const Real downside = std::sqrt((0.0001 + 0.0004) / 4.0);
        EXPECT_NEAR(sortino_ratio(excess), 0.005 / downside, 1e-12);
    }

    TEST(Metrics, DrawdownByHand)
    {
        const std::vector<Real> values = {100.0, 120.0, 90.0, 96.0, 120.0, 130.0, 117.0};
        const std::vector<Real> series = drawdowns(values);
        EXPECT_DOUBLE_EQ(series[0], 0.0);
        EXPECT_DOUBLE_EQ(series[2], -0.25);
        EXPECT_DOUBLE_EQ(series[6], -0.10);
        EXPECT_DOUBLE_EQ(max_drawdown(values), -0.25);
        // Below the high of 120 on two periods, then back to it.
        EXPECT_EQ(longest_underwater(values), 2u);
        const std::vector<Real> returns = simple_returns(values);
        EXPECT_NEAR(calmar_ratio(returns, 6.0), 0.17 / 0.25, 1e-12);
    }

    TEST(Metrics, HistoricalVarAndExpectedShortfallByHand)
    {
        // Twenty returns: -10 %, -9 %, ..., +9 %.
        std::vector<Real> returns;
        for (int k = 0; k < 20; ++k)
        {
            returns.push_back(0.01 * (k - 10));
        }
        std::swap(returns[0], returns[13]);
        // 10 % tail: the two worst, -10 % and -9 %.
        EXPECT_NEAR(historical_var(returns, 0.10), 0.09, 1e-12);
        EXPECT_NEAR(historical_expected_shortfall(returns, 0.10), 0.095, 1e-12);
        // 5 % tail: the worst alone.
        EXPECT_NEAR(historical_var(returns, 0.05), 0.10, 1e-12);
        EXPECT_NEAR(historical_expected_shortfall(returns, 0.05), 0.10, 1e-12);
        EXPECT_GE(historical_expected_shortfall(returns, 0.25), historical_var(returns, 0.25));
        EXPECT_THROW(historical_var(returns, 0.0), Error);
    }

    TEST(Metrics, GaussianVarMatchesTheQuantile)
    {
        const std::vector<Real> returns = gaussian(200000, 0.0, 0.01, 8);
        EXPECT_NEAR(historical_var(returns, 0.05), 0.01 * 1.6449, 2e-4);
        // Gaussian ES at 5 %: density at the quantile over the tail probability.
        EXPECT_NEAR(historical_expected_shortfall(returns, 0.05), 0.01 * 2.0627, 3e-4);
    }

    TEST(Metrics, TrackingErrorAndInformationRatio)
    {
        const std::vector<Real> benchmark = gaussian(1000, 0.0004, 0.01, 9);
        const std::vector<Real> active = gaussian(1000, 0.0002, 0.002, 10);
        std::vector<Real> returns(benchmark.size());
        for (Index t = 0; t < returns.size(); ++t)
        {
            returns[t] = benchmark[t] + active[t];
        }
        EXPECT_NEAR(tracking_error(returns, benchmark, 252.0), standard_deviation(active) * std::sqrt(252.0), 1e-12);
        EXPECT_NEAR(information_ratio(returns, benchmark), sharpe_ratio(active), 1e-9);
        EXPECT_THROW(tracking_error(returns, std::vector<Real>{0.0}, 252.0), Error);
    }

    TEST(Metrics, WinRateAndGainLossRatio)
    {
        const std::vector<Real> returns = {0.02, -0.01, 0.04, 0.0, -0.03};
        EXPECT_DOUBLE_EQ(win_rate(returns), 0.4);
        EXPECT_NEAR(gain_loss_ratio(returns), 0.03 / 0.02, 1e-12);
        EXPECT_TRUE(std::isnan(gain_loss_ratio(std::vector<Real>{0.01, 0.02})));
    }

    TEST(Metrics, ReturnsByCalendarYear)
    {
        // 2019-12-30, 2019-12-31, 2020-01-02, 2020-01-03.
        const std::vector<Date> dates = {18260, 18261, 18263, 18264};
        const std::vector<Real> returns = {0.01, 0.02, -0.10, 0.05};
        const std::vector<YearReturn> years = calendar_year_returns(dates, returns);
        ASSERT_EQ(years.size(), 2u);
        EXPECT_EQ(years[0].year, 2019);
        EXPECT_NEAR(years[0].value, 1.01 * 1.02 - 1.0, 1e-12);
        EXPECT_EQ(years[0].observations, 2u);
        EXPECT_EQ(years[1].year, 2020);
        EXPECT_NEAR(years[1].value, 0.90 * 1.05 - 1.0, 1e-12);
        EXPECT_THROW(calendar_year_returns(dates, std::vector<Real>{0.1}), Error);
    }

} // namespace quantBacktest
