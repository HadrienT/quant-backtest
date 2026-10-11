#include <gtest/gtest.h>

#include "quantBacktest/core/error.hpp"
#include "quantBacktest/costs/cost_model.hpp"
#include "quantBacktest/costs/impact.hpp"
#include "quantBacktest/engine/backtest.hpp"
#include "quantBacktest/costs/spread_estimators.hpp"

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

        struct Bars
        {
            std::vector<Real> high;
            std::vector<Real> low;
            std::vector<Real> close;
        };

        // The setting of Corwin and Schultz's simulations: an efficient price
        // that diffuses during the session (no overnight move), and each
        // trade observed at the bid or at the ask with equal probability.
        Bars simulateBars(Index days, Real spread, Real dailyVolatility, std::uint64_t seed)
        {
            const Index tradesPerDay = 390;
            std::mt19937_64 rng(seed);
            std::normal_distribution<Real> normal(0.0, dailyVolatility / std::sqrt(static_cast<Real>(tradesPerDay)));
            std::bernoulli_distribution buy(0.5);
            Bars bars;
            Real mid = 100.0;
            for (Index t = 0; t < days; ++t)
            {
                Real high = 0.0;
                Real low = std::numeric_limits<Real>::infinity();
                Real last = mid;
                for (Index k = 0; k < tradesPerDay; ++k)
                {
                    mid *= std::exp(normal(rng));
                    last = mid * (buy(rng) ? 1.0 + 0.5 * spread : 1.0 - 0.5 * spread);
                    high = std::max(high, last);
                    low = std::min(low, last);
                }
                bars.high.push_back(high);
                bars.low.push_back(low);
                bars.close.push_back(last);
            }
            return bars;
        }

    } // namespace

    // Property: on simulated trades with a known spread, each estimator
    // returns that spread, when the spread is wide next to the daily
    // volatility (1 % here).
    TEST(SpreadEstimators, RecoverAWideSimulatedSpread)
    {
        for (const Real spread : {0.03, 0.05})
        {
            const Bars bars = simulateBars(4000, spread, 0.01, 11);
            const Real corwinSchultz = corwin_schultz_spread(bars.high, bars.low, bars.close);
            const Real abdiRanaldo = abdi_ranaldo_spread(bars.high, bars.low, bars.close);
            EXPECT_NEAR(corwinSchultz, spread, 0.05 * spread) << "Corwin-Schultz, true spread " << spread;
            EXPECT_NEAR(abdiRanaldo, spread, 0.05 * spread) << "Abdi-Ranaldo, true spread " << spread;
        }
    }

    // The known limit of both estimators, kept visible: when the spread is
    // narrow next to the daily volatility, the estimate is noise, of the order
    // of a fraction of that volatility, far above the true spread. Here a
    // liquid large capitalisation: 3 basis points of spread, 1.5 % of daily
    // volatility, monthly windows as the cost model uses them.
    TEST(SpreadEstimators, OverstateANarrowSpread)
    {
        const Real spread = 0.0003;
        const Real volatility = 0.015;
        const Bars bars = simulateBars(4200, spread, volatility, 11);
        Real corwinSchultz = 0.0;
        Real abdiRanaldo = 0.0;
        const Index window = 21;
        const Index windows = bars.high.size() / window;
        for (Index w = 0; w < windows; ++w)
        {
            const std::span<const Real> high(bars.high.data() + w * window, window);
            const std::span<const Real> low(bars.low.data() + w * window, window);
            const std::span<const Real> close(bars.close.data() + w * window, window);
            corwinSchultz += corwin_schultz_spread(high, low, close) / static_cast<Real>(windows);
            abdiRanaldo += abdi_ranaldo_spread(high, low, close) / static_cast<Real>(windows);
        }
        EXPECT_GT(corwinSchultz, 10.0 * spread);
        EXPECT_GT(abdiRanaldo, 5.0 * spread);
        EXPECT_NEAR(corwinSchultz, 0.4 * volatility, 0.1 * volatility);
        EXPECT_NEAR(abdiRanaldo, 0.2 * volatility, 0.1 * volatility);
    }

    TEST(SpreadEstimators, WiderSpreadGivesAHigherEstimate)
    {
        Real previousCs = 0.0;
        Real previousAr = 0.0;
        for (const Real spread : {0.01, 0.02, 0.05})
        {
            const Bars bars = simulateBars(2000, spread, 0.01, 5);
            const Real cs = corwin_schultz_spread(bars.high, bars.low, bars.close);
            const Real ar = abdi_ranaldo_spread(bars.high, bars.low, bars.close);
            EXPECT_GT(cs, previousCs);
            EXPECT_GT(ar, previousAr);
            previousCs = cs;
            previousAr = ar;
        }
    }

    TEST(SpreadEstimators, DoNotDependOnThePriceUnit)
    {
        Bars bars = simulateBars(300, 0.01, 0.02, 3);
        const Real cs = corwin_schultz_spread(bars.high, bars.low, bars.close);
        const Real ar = abdi_ranaldo_spread(bars.high, bars.low, bars.close);
        for (std::vector<Real> *series : {&bars.high, &bars.low, &bars.close})
        {
            for (Real &x : *series)
            {
                x *= 0.25;
            }
        }
        EXPECT_NEAR(corwin_schultz_spread(bars.high, bars.low, bars.close), cs, 1e-12);
        EXPECT_NEAR(abdi_ranaldo_spread(bars.high, bars.low, bars.close), ar, 1e-12);
    }

    TEST(SpreadEstimators, MissingDaysAreSkippedAndTooFewPairsGiveNoEstimate)
    {
        Bars bars = simulateBars(30, 0.01, 0.02, 3);
        bars.close[10] = NaN;
        // Day 10 is in two pairs: 27 remain out of 29.
        EXPECT_FALSE(std::isnan(corwin_schultz_spread(bars.high, bars.low, bars.close, 27)));
        EXPECT_TRUE(std::isnan(corwin_schultz_spread(bars.high, bars.low, bars.close, 28)));
        EXPECT_FALSE(std::isnan(abdi_ranaldo_spread(bars.high, bars.low, bars.close, 27)));
        EXPECT_TRUE(std::isnan(abdi_ranaldo_spread(bars.high, bars.low, bars.close, 28)));

        const std::vector<Real> one = {100.0};
        EXPECT_TRUE(std::isnan(corwin_schultz_spread(one, one, one)));
        EXPECT_TRUE(std::isnan(abdi_ranaldo_spread(one, one, one)));
        EXPECT_THROW(corwin_schultz_spread(bars.high, bars.low, one), Error);
    }

    TEST(ProportionalCost, IsTheRateTimesTheAmount)
    {
        const testing::SyntheticMarket m = testing::makeMarket(10, 2, 1);
        const MarketData data = m.data();
        const EventIndex events(data);
        const MarketView view(data, events, 5);
        const ProportionalCost cost(0.0005);
        EXPECT_DOUBLE_EQ(cost.cost(view, {5, 0, 200.0, 50.0}), 5.0);
        EXPECT_DOUBLE_EQ(cost.cost(view, {5, 0, -200.0, 50.0}), 5.0);
        EXPECT_NE(cost.describe().find("5 basis points"), std::string::npos);
        EXPECT_THROW(ProportionalCost(-0.001), Error);
        EXPECT_THROW(ProportionalCost(std::numeric_limits<Real>::quiet_NaN()), Error);
    }

    // The table of a whole market is the per-asset estimator applied to the
    // window that precedes each day.
    TEST(EstimatedSpreads, RowTIsTheEstimateOverTheWindowBeforeT)
    {
        testing::SyntheticMarket m = testing::makeMarket(80, 3, 1);
        m.high[m.at(33, 2)] = NaN;
        m.low[m.at(33, 2)] = NaN;
        m.close[m.at(33, 2)] = NaN;
        m.open[m.at(33, 2)] = NaN;
        const Index window = 21;
        for (const SpreadEstimator estimator : {SpreadEstimator::CorwinSchultz, SpreadEstimator::AbdiRanaldo})
        {
            const std::vector<Real> table = estimated_spreads(m.data(), estimator, window, 10);
            ASSERT_EQ(table.size(), m.days * m.assets);
            for (Index t = 0; t < m.days; ++t)
            {
                for (Index i = 0; i < m.assets; ++i)
                {
                    if (t < window)
                    {
                        // Not enough history: no estimate.
                        EXPECT_TRUE(std::isnan(table[m.at(t, i)]));
                        continue;
                    }
                    std::vector<Real> high;
                    std::vector<Real> low;
                    std::vector<Real> close;
                    for (Index s = t - window; s < t; ++s)
                    {
                        high.push_back(m.high[m.at(s, i)]);
                        low.push_back(m.low[m.at(s, i)]);
                        close.push_back(m.close[m.at(s, i)]);
                    }
                    const Real expected = estimator == SpreadEstimator::CorwinSchultz ? corwin_schultz_spread(high, low, close, 10)
                                                                                      : abdi_ranaldo_spread(high, low, close, 10);
                    EXPECT_EQ(table[m.at(t, i)], expected) << "day " << t << ", asset " << i;
                }
            }
        }
        testing::SyntheticMarket bare = m;
        bare.high.clear();
        bare.low.clear();
        EXPECT_THROW(estimated_spreads(bare.data(), SpreadEstimator::CorwinSchultz, window, 10), Error);
        EXPECT_THROW(estimated_spreads(m.data(), SpreadEstimator::CorwinSchultz, 1, 1), Error);
        EXPECT_THROW(estimated_spreads(m.data(), SpreadEstimator::CorwinSchultz, 21, 21), Error);
    }

    // Row t is known before trading on day t: it does not move with the bars
    // of t or of any later day.
    TEST(EstimatedSpreads, RowTDoesNotReadDayT)
    {
        const testing::SyntheticMarket m = testing::makeMarket(60, 2, 2);
        testing::SyntheticMarket changed = m;
        for (Index t = 40; t < m.days; ++t)
        {
            for (Index i = 0; i < m.assets; ++i)
            {
                changed.high[changed.at(t, i)] *= 1.5;
                changed.close[changed.at(t, i)] *= 1.2;
            }
        }
        for (const SpreadEstimator estimator : {SpreadEstimator::CorwinSchultz, SpreadEstimator::AbdiRanaldo})
        {
            const std::vector<Real> a = estimated_spreads(m.data(), estimator, 21, 5);
            const std::vector<Real> b = estimated_spreads(changed.data(), estimator, 21, 5);
            for (Index k = 21 * m.assets; k < 41 * m.assets; ++k)
            {
                ASSERT_EQ(a[k], b[k]);
            }
            EXPECT_NE(a[m.at(45, 0)], b[m.at(45, 0)]);
        }
    }

    // A split inside the window is undone: the estimate is the one of the
    // unsplit prices.
    TEST(EstimatedSpreads, AreInvariantToASplit)
    {
        const testing::SyntheticMarket m = testing::makeMarket(60, 2, 2);
        testing::SyntheticMarket split = m;
        split.addSplit(0, 30, 2.0);
        split.addSplit(1, 31, 0.2);
        for (const SpreadEstimator estimator : {SpreadEstimator::CorwinSchultz, SpreadEstimator::AbdiRanaldo})
        {
            const std::vector<Real> a = estimated_spreads(m.data(), estimator, 21, 5);
            const std::vector<Real> b = estimated_spreads(split.data(), estimator, 21, 5);
            Real total = 0.0;
            for (Index k = 21 * m.assets; k < a.size(); ++k)
            {
                ASSERT_NEAR(a[k], b[k], 1e-12);
                total += a[k];
            }
            EXPECT_GT(total, 0.0);
        }
    }

    TEST(SpreadCost, IsHalfTheSpreadOnTheAmountTraded)
    {
        const testing::SyntheticMarket m = testing::makeMarket(10, 2, 1);
        const MarketData data = m.data();
        const EventIndex events(data);
        const MarketView view(data, events, 5);
        std::vector<Real> spreads(m.days * m.assets, 0.002);
        spreads[m.at(5, 1)] = NaN;
        const SpreadCost model({spreads.data(), m.days, m.assets}, "quoted");
        EXPECT_DOUBLE_EQ(model.cost(view, {5, 0, -10.0, 100.0}), 0.5 * 0.002 * 1000.0);
        // No spread, no cost estimate: the engine rejects the order.
        EXPECT_TRUE(std::isnan(model.cost(view, {5, 1, 10.0, 100.0})));
        EXPECT_NE(model.describe().find("quoted"), std::string::npos);
        EXPECT_NO_THROW(model.validate(data));

        const SpreadCost misshapen({spreads.data(), m.days - 1, m.assets}, "quoted");
        EXPECT_THROW(misshapen.validate(data), Error);
        EXPECT_THROW(SpreadCost(MatrixView{}, "nothing"), Error);
        spreads[m.at(5, 0)] = -0.001;
        EXPECT_THROW(model.cost(view, {5, 0, 10.0, 100.0}), Error);
    }

    TEST(CostStack, AddsItsModels)
    {
        const testing::SyntheticMarket m = testing::makeMarket(60, 1, 2);
        const MarketData data = m.data();
        const EventIndex events(data);
        const MarketView view(data, events, 40);
        const ProportionalCost commission(0.0002);
        const std::vector<Real> spreads = estimated_spreads(data, SpreadEstimator::AbdiRanaldo, 21, 5);
        const SpreadCost spread({spreads.data(), m.days, m.assets}, "Abdi and Ranaldo");
        const CostStack stack({&commission, &spread});
        const Fill fill{40, 0, 100.0, 80.0};
        EXPECT_DOUBLE_EQ(stack.cost(view, fill), commission.cost(view, fill) + spread.cost(view, fill));
        EXPECT_NE(stack.describe().find("plus"), std::string::npos);
        EXPECT_THROW(CostStack({&commission, nullptr}), Error);
    }

    // ---- Market impact ----

    TEST(ImpactTables, ReadOnlyTheDaysBeforeAndIgnoreSplits)
    {
        const testing::SyntheticMarket m = testing::makeMarket(80, 3, 4);
        const std::vector<Real> volatility = trailing_volatility(m.data(), 21, 10);
        const std::vector<Real> volume = trailing_dollar_volume(m.data(), 21, 10);
        // Row 40 by hand, asset 1: returns of days 19 to 39.
        std::vector<Real> returns;
        Real amount = 0.0;
        for (Index s = 19; s < 40; ++s)
        {
            returns.push_back(m.close[m.at(s, 1)] / m.close[m.at(s - 1, 1)] - 1.0);
            amount += m.close[m.at(s, 1)] * m.volume[m.at(s, 1)] / 21.0;
        }
        Real mean = 0.0;
        for (const Real r : returns)
        {
            mean += r / 21.0;
        }
        Real squares = 0.0;
        for (const Real r : returns)
        {
            squares += (r - mean) * (r - mean);
        }
        EXPECT_NEAR(volatility[m.at(40, 1)], std::sqrt(squares / 20.0), 1e-12);
        EXPECT_NEAR(volume[m.at(40, 1)], amount, 1e-6 * amount);
        EXPECT_TRUE(std::isnan(volatility[m.at(20, 1)]));
        EXPECT_TRUE(std::isnan(volume[m.at(20, 1)]));

        // Day 40 and after do not enter row 40.
        testing::SyntheticMarket changed = m;
        for (Index t = 40; t < m.days; ++t)
        {
            changed.close[changed.at(t, 1)] *= 1.3;
            changed.volume[changed.at(t, 1)] *= 5.0;
        }
        EXPECT_EQ(trailing_volatility(changed.data(), 21, 10)[m.at(40, 1)], volatility[m.at(40, 1)]);
        EXPECT_EQ(trailing_dollar_volume(changed.data(), 21, 10)[m.at(40, 1)], volume[m.at(40, 1)]);

        // A split changes neither: prices halve, volumes double.
        testing::SyntheticMarket split = m;
        split.addSplit(1, 30, 2.0);
        const std::vector<Real> splitVolatility = trailing_volatility(split.data(), 21, 10);
        const std::vector<Real> splitVolume = trailing_dollar_volume(split.data(), 21, 10);
        for (Index t = 21; t < m.days; ++t)
        {
            ASSERT_NEAR(splitVolatility[m.at(t, 1)], volatility[m.at(t, 1)], 1e-12);
            ASSERT_NEAR(splitVolume[m.at(t, 1)], volume[m.at(t, 1)], 1e-9 * volume[m.at(t, 1)]);
        }

        testing::SyntheticMarket bare = m;
        bare.volume.clear();
        EXPECT_THROW(trailing_dollar_volume(bare.data(), 21, 10), Error);
        EXPECT_THROW(trailing_volatility(m.data(), 1, 1), Error);
    }

    // The cost grows as the power 3/2 of the size: twice the order costs
    // 2.83 times as much.
    TEST(SquareRootImpactCost, GrowsAsThePowerThreeHalvesOfTheSize)
    {
        const testing::SyntheticMarket m = testing::makeMarket(10, 2, 1);
        const MarketData data = m.data();
        const EventIndex events(data);
        const MarketView view(data, events, 5);
        std::vector<Real> volatility(m.days * m.assets, 0.02);
        std::vector<Real> volume(m.days * m.assets, 1e7);
        volume[m.at(5, 1)] = NaN;
        const SquareRootImpactCost impact({volatility.data(), m.days, m.assets}, {volume.data(), m.days, m.assets}, 1.0);
        // 1 % of the daily amount: 0.02 x sqrt(0.01) = 20 basis points.
        EXPECT_NEAR(impact.cost(view, {5, 0, 1000.0, 100.0}), 1e5 * 0.002, 1e-9);
        EXPECT_NEAR(impact.cost(view, {5, 0, -2000.0, 100.0}) / impact.cost(view, {5, 0, 1000.0, 100.0}), std::pow(2.0, 1.5), 1e-12);
        EXPECT_TRUE(std::isnan(impact.cost(view, {5, 1, 1000.0, 100.0})));
        EXPECT_NO_THROW(impact.validate(data));
        EXPECT_NE(impact.describe().find("square-root"), std::string::npos);
        EXPECT_THROW(SquareRootImpactCost(MatrixView{}, {volume.data(), m.days, m.assets}, 1.0), Error);
        EXPECT_THROW(SquareRootImpactCost({volatility.data(), m.days, m.assets}, {volume.data(), m.days, m.assets}, -1.0), Error);
        const SquareRootImpactCost misshapen({volatility.data(), m.days - 1, m.assets}, {volume.data(), m.days, m.assets}, 1.0);
        EXPECT_THROW(misshapen.validate(data), Error);
    }

    // Capacity: with impact, a strategy does not scale. Ten times the capital
    // pays more than ten times the costs, and trades a larger share of the
    // daily amount.
    TEST(SquareRootImpactCost, MakesAStrategyLoseItsScaleInvariance)
    {
        const testing::SyntheticMarket m = testing::makeMarket(300, 6, 9);
        const MarketData data = m.data();
        const std::vector<Real> volatility = trailing_volatility(data, 21, 10);
        const std::vector<Real> volume = trailing_dollar_volume(data, 21, 10);
        const MatrixView volumeView{volume.data(), m.days, m.assets};
        const SquareRootImpactCost impact({volatility.data(), m.days, m.assets}, volumeView, 1.0);
        const auto run = [&](Real capital)
        {
            testing::Momentum strategy(20, 5);
            BacktestConfig config;
            config.initial_capital = capital;
            config.costs = &impact;
            config.start_index = 30;
            return run_backtest(data, strategy, config);
        };
        const BacktestResult small = run(1e6);
        const BacktestResult large = run(1e7);
        Real smallCosts = 0.0;
        Real largeCosts = 0.0;
        for (Index k = 0; k < small.costs.size(); ++k)
        {
            smallCosts += small.costs[k];
            largeCosts += large.costs[k];
        }
        ASSERT_GT(smallCosts, 0.0);
        // 10^1.5 = 31.6, less what the costs themselves take off the book.
        EXPECT_GT(largeCosts, 25.0 * smallCosts);
        EXPECT_LT(large.value.back() / 1e7, small.value.back() / 1e6);

        const Participation smallShare = participation(small, volumeView, m.dates);
        const Participation largeShare = participation(large, volumeView, m.dates);
        EXPECT_GT(smallShare.executions, 50u);
        EXPECT_GE(smallShare.maximum, smallShare.median);
        EXPECT_NEAR(largeShare.median / smallShare.median, 10.0, 1.0);
        EXPECT_THROW(participation(small, MatrixView{}, m.dates), Error);
    }

} // namespace quantBacktest
