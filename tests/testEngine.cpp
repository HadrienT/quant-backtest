// Property tests of the companion, section 16, and findings 2 and 8 of its
// section 2.
#include <gtest/gtest.h>

#include "quantBacktest/core/error.hpp"
#include "quantBacktest/costs/spread_estimators.hpp"
#include "quantBacktest/data/returns.hpp"
#include "quantBacktest/engine/backtest.hpp"
#include "quantBacktest/engine/vectorized.hpp"

#include "support/synthetic_market.hpp"

#include <atomic>
#include <cmath>
#include <limits>
#include <random>
#include <vector>

namespace quantBacktest
{

    namespace
    {

        using testing::SyntheticMarket;

        constexpr Real NaN = std::numeric_limits<Real>::quiet_NaN();
        constexpr Real Capital = 1e6;

        std::vector<Real> equalWeights(Index assets, Real invested = 1.0)
        {
            return std::vector<Real>(assets, invested / static_cast<Real>(assets));
        }

        BacktestConfig config(ExecutionTiming timing = ExecutionTiming::NextClose, const CostModel *costs = nullptr)
        {
            BacktestConfig c;
            c.initial_capital = Capital;
            c.timing = timing;
            c.costs = costs;
            return c;
        }

        // Spreads estimated from the market's own bars, and the cost that
        // reads them.
        struct EstimatedSpread
        {
            EstimatedSpread(const SyntheticMarket &m, SpreadEstimator estimator)
                : table(estimated_spreads(m.data(), estimator, 21, 10)),
                  cost({table.data(), m.days, m.assets}, "estimated over the 21 days before the execution") {}

            std::vector<Real> table;
            SpreadCost cost;
        };

        // A market with events of every kind: dividends, a split, a reverse
        // split, interest on cash.
        SyntheticMarket eventfulMarket(Index days = 400, Index assets = 6, std::uint64_t seed = 42)
        {
            SyntheticMarket m = testing::makeMarket(days, assets, seed);
            m.addSplit(1, days / 3, 2.0);
            m.addSplit(3, days / 2, 0.25);
            return m;
        }

        // Targets the rows of a table: row t is decided on day t.
        class Scheduled final : public Strategy
        {
          public:
            Scheduled(const std::vector<Real> &targets, Index assets)
                : targets_(targets), assets_(assets) {}

            void decide(const MarketView &market, const BookView &, Decision &decision) override
            {
                const Index t = market.day();
                if ((t + 1) * assets_ <= targets_.size())
                {
                    decision.kind = DecisionKind::TargetWeights;
                    decision.values.assign(targets_.begin() + static_cast<std::ptrdiff_t>(t * assets_),
                                           targets_.begin() + static_cast<std::ptrdiff_t>((t + 1) * assets_));
                }
            }

          private:
            const std::vector<Real> &targets_;
            Index assets_;
        };

        // One decision, in shares or in weights, on a given day.
        class Once final : public Strategy
        {
          public:
            Once(Index day, DecisionKind kind, std::vector<Real> values)
                : day_(day), kind_(kind), values_(std::move(values)) {}

            void decide(const MarketView &market, const BookView &, Decision &decision) override
            {
                if (market.day() == day_)
                {
                    decision.kind = kind_;
                    decision.values = values_;
                }
            }

          private:
            Index day_;
            DecisionKind kind_;
            std::vector<Real> values_;
        };

        void expectConservation(const SyntheticMarket &m, const BacktestResult &r, Index start = 0)
        {
            ASSERT_EQ(r.value.size(), m.days - start);
            Real previous = Capital;
            for (Index k = 0; k < r.value.size(); ++k)
            {
                const Real tolerance = 1e-9 * std::abs(r.value[k]);
                Real positions = 0.0;
                for (Index i = 0; i < m.assets; ++i)
                {
                    const Real shares = r.shares[k * m.assets + i];
                    if (shares != 0.0)
                    {
                        positions += shares * m.close[m.at(start + k, i)];
                    }
                }
                ASSERT_NEAR(r.value[k], r.cash[k] + positions, tolerance) << "value on day " << k;
                ASSERT_NEAR(r.value[k] - previous,
                            r.position_pnl[k] + r.dividends[k] + r.interest[k] - r.costs[k], tolerance)
                    << "change of value on day " << k;
                previous = r.value[k];
            }
        }

    } // namespace

    // ---- Section 16: conservation ----

    TEST(Engine, ConservationWithEventsAndCosts)
    {
        const SyntheticMarket m = eventfulMarket();
        const ProportionalCost commission(0.0005);
        const EstimatedSpread spread(m, SpreadEstimator::CorwinSchultz);
        const CostStack costs({&commission, &spread.cost});
        for (const ExecutionTiming timing : {ExecutionTiming::NextOpen, ExecutionTiming::NextClose, ExecutionTiming::SameClose})
        {
            testing::Momentum strategy(20, 5);
            const BacktestResult r = run_backtest(m.data(), strategy, config(timing, &costs));
            expectConservation(m, r);
            Real paid = 0.0;
            Real dividends = 0.0;
            Real interest = 0.0;
            for (Index k = 0; k < r.value.size(); ++k)
            {
                paid += r.costs[k];
                dividends += r.dividends[k];
                interest += r.interest[k];
            }
            EXPECT_GT(paid, 0.0);
            EXPECT_GT(dividends, 0.0);
            EXPECT_GT(interest, 0.0);
            Real journalCosts = 0.0;
            for (const OrderRecord &order : r.journal)
            {
                journalCosts += order.cost;
            }
            EXPECT_NEAR(journalCosts, paid, 1e-9 * paid);
        }
    }

    // ---- Section 16: no future ----

    TEST(Engine, ChangingTheFutureChangesNothingBefore)
    {
        SyntheticMarket m = eventfulMarket();
        const Index cut = 250;
        // Gaps in the quotes, some of them on the last common day: what fills
        // them must not come from the other side.
        const auto removeQuote = [&](Index t, Index asset)
        {
            for (std::vector<Real> *table : {&m.open, &m.high, &m.low, &m.close})
            {
                (*table)[m.at(t, asset)] = NaN;
            }
        };
        for (const Index t : {Index{100}, Index{101}, Index{102}})
        {
            removeQuote(t, 4);
        }
        for (const Index asset : {Index{0}, Index{2}, Index{4}, Index{5}})
        {
            removeQuote(cut, asset);
        }
        // Same history up to `cut`, another world after it: prices, a new
        // dividend and a new split.
        SyntheticMarket other = m;
        std::mt19937_64 rng(1);
        std::uniform_real_distribution<Real> shock(0.5, 1.5);
        for (Index t = cut + 1; t < m.days; ++t)
        {
            for (Index i = 0; i < m.assets; ++i)
            {
                const Real factor = shock(rng);
                for (std::vector<Real> *table : {&other.open, &other.high, &other.low, &other.close})
                {
                    (*table)[other.at(t, i)] *= factor;
                }
            }
            other.cashRate[t] *= 3.0;
        }
        other.dividends.push_back({0, m.dates[cut + 1], 5.0});
        other.addSplit(2, cut + 2, 3.0);
        // And the series simply stopping there.
        SyntheticMarket truncated = m;
        truncated.days = cut + 1;
        truncated.dates.resize(cut + 1);
        truncated.cashRate.resize(cut + 1);
        for (std::vector<Real> *table : {&truncated.open, &truncated.high, &truncated.low, &truncated.close, &truncated.volume})
        {
            table->resize((cut + 1) * m.assets);
        }

        // Each world estimates its spreads from its own bars.
        const ProportionalCost commission(0.0005);
        const EstimatedSpread spread(m, SpreadEstimator::AbdiRanaldo);
        const EstimatedSpread otherSpread(other, SpreadEstimator::AbdiRanaldo);
        const EstimatedSpread truncatedSpread(truncated, SpreadEstimator::AbdiRanaldo);
        const CostStack costs({&commission, &spread.cost});
        const CostStack otherCosts({&commission, &otherSpread.cost});
        const CostStack truncatedCosts({&commission, &truncatedSpread.cost});
        for (const ExecutionTiming timing : {ExecutionTiming::NextOpen, ExecutionTiming::NextClose, ExecutionTiming::SameClose})
        {
            testing::Momentum s1(20, 5);
            testing::Momentum s2(20, 5);
            testing::Momentum s3(20, 5);
            const BacktestResult base = run_backtest(m.data(), s1, config(timing, &costs));
            for (const BacktestResult &alt : {run_backtest(other.data(), s2, config(timing, &otherCosts)),
                                              run_backtest(truncated.data(), s3, config(timing, &truncatedCosts))})
            {
                // Bit for bit, not "close to".
                for (Index t = 0; t <= cut; ++t)
                {
                    ASSERT_EQ(alt.value[t], base.value[t]) << "day " << t;
                    ASSERT_EQ(alt.cash[t], base.cash[t]) << "day " << t;
                    ASSERT_EQ(alt.costs[t], base.costs[t]) << "day " << t;
                    for (Index i = 0; i < m.assets; ++i)
                    {
                        ASSERT_EQ(alt.shares[t * m.assets + i], base.shares[t * m.assets + i]);
                    }
                }
                Index compared = 0;
                for (Index k = 0; k < base.journal.size() && base.journal[k].execution_date <= m.dates[cut]; ++k)
                {
                    ASSERT_LT(k, alt.journal.size());
                    EXPECT_EQ(alt.journal[k].decision_date, base.journal[k].decision_date);
                    EXPECT_EQ(alt.journal[k].asset, base.journal[k].asset);
                    EXPECT_EQ(alt.journal[k].status, base.journal[k].status);
                    EXPECT_EQ(alt.journal[k].quantity, base.journal[k].quantity);
                    // A rejected order has no price: NaN on both sides.
                    EXPECT_TRUE(alt.journal[k].price == base.journal[k].price ||
                                (std::isnan(alt.journal[k].price) && std::isnan(base.journal[k].price)));
                    ++compared;
                }
                EXPECT_GT(compared, 50u);
                // The gap of the last common day was a held position.
                EXPECT_GT(base.stale_positions[cut], 0u);
            }
        }
    }

    // Inside the day too: an order executed at the open of day t owes nothing
    // to the high, the low or the close of that day.
    TEST(Engine, AnOpenExecutionDoesNotSeeTheRestOfItsDay)
    {
        const SyntheticMarket m = eventfulMarket();
        const ProportionalCost commission(0.0005);
        const EstimatedSpread spread(m, SpreadEstimator::CorwinSchultz);
        const CostStack costs({&commission, &spread.cost});
        testing::Momentum s1(20, 5);
        const BacktestResult base = run_backtest(m.data(), s1, config(ExecutionTiming::NextOpen, &costs));

        // Decisions fall on multiples of 5: day 201 is an execution day.
        const Index day = 201;
        SyntheticMarket other = m;
        for (Index i = 0; i < m.assets; ++i)
        {
            other.close[other.at(day, i)] *= 1.2;
            other.high[other.at(day, i)] *= 1.3;
            other.low[other.at(day, i)] *= 0.9;
        }
        const EstimatedSpread otherSpread(other, SpreadEstimator::CorwinSchultz);
        const CostStack otherCosts({&commission, &otherSpread.cost});
        testing::Momentum s2(20, 5);
        const BacktestResult alt = run_backtest(other.data(), s2, config(ExecutionTiming::NextOpen, &otherCosts));

        Index fills = 0;
        for (Index k = 0; k < base.journal.size() && base.journal[k].execution_date <= m.dates[day]; ++k)
        {
            ASSERT_EQ(alt.journal[k].quantity, base.journal[k].quantity);
            ASSERT_EQ(alt.journal[k].price, base.journal[k].price);
            ASSERT_EQ(alt.journal[k].cost, base.journal[k].cost);
            fills += base.journal[k].execution_date == m.dates[day] ? 1 : 0;
        }
        EXPECT_GT(fills, 0u);
        EXPECT_EQ(alt.cash[day], base.cash[day]);
        EXPECT_NE(alt.value[day], base.value[day]);
    }

    // A strategy that tries to read tomorrow is stopped by the view.
    TEST(Engine, AStrategyCannotReadTomorrow)
    {
        class Cheater final : public Strategy
        {
            void decide(const MarketView &market, const BookView &, Decision &) override
            {
                (void)market.close(market.day() + 1, 0);
            }
        };
        const SyntheticMarket m = testing::makeMarket(30, 2, 1);
        Cheater cheater;
        EXPECT_THROW(run_backtest(m.data(), cheater, config()), Error);
    }

    // ---- Section 16: zero costs, the event-driven engine equals the vectorized computation ----

    TEST(Engine, WithoutCostsMatchesTheVectorizedBacktest)
    {
        const SyntheticMarket m = eventfulMarket(300, 5, 9);
        // Weights that change every day, never fully invested.
        std::mt19937_64 rng(3);
        std::uniform_real_distribution<Real> uniform(0.0, 1.0);
        std::vector<Real> targets(m.days * m.assets);
        for (Index t = 0; t < m.days; ++t)
        {
            Real sum = 0.0;
            for (Index i = 0; i < m.assets; ++i)
            {
                targets[m.at(t, i)] = uniform(rng);
                sum += targets[m.at(t, i)];
            }
            const Real invested = 0.5 + 0.5 * uniform(rng);
            for (Index i = 0; i < m.assets; ++i)
            {
                targets[m.at(t, i)] *= invested / sum;
            }
        }
        Scheduled strategy(targets, m.assets);
        const BacktestResult r = run_backtest(m.data(), strategy, config(ExecutionTiming::NextClose));

        // Decided on day t, executed at the close of t+1, held during t+2.
        std::vector<Real> held(m.days * m.assets, 0.0);
        for (Index t = 2; t < m.days; ++t)
        {
            for (Index i = 0; i < m.assets; ++i)
            {
                held[m.at(t, i)] = targets[m.at(t - 2, i)];
            }
        }
        const std::vector<Real> returns = total_returns(m.data());
        const std::vector<Real> vectorized = vectorized_values({returns.data(), m.days, m.assets}, {held.data(), m.days, m.assets}, m.cashRate, Capital);

        for (Index t = 0; t < m.days; ++t)
        {
            ASSERT_NEAR(r.value[t], vectorized[t], 1e-10 * vectorized[t]) << "day " << t;
        }
        EXPECT_NE(r.value.back(), Capital);
    }

    // ---- Section 16: cost monotonicity ----

    TEST(Engine, FinalValueDecreasesWithTheCostRate)
    {
        const SyntheticMarket m = eventfulMarket();
        Real previous = std::numeric_limits<Real>::infinity();
        for (const Real rate : {0.0, 0.0001, 0.0005, 0.002, 0.01})
        {
            const ProportionalCost cost(rate);
            testing::ConstantWeights strategy(equalWeights(m.assets), 5);
            const BacktestResult r = run_backtest(m.data(), strategy, config(ExecutionTiming::NextClose, &cost));
            EXPECT_LT(r.value.back(), previous) << "rate " << rate;
            previous = r.value.back();
        }
    }

    // ---- Section 16: split invariance ----

    TEST(Engine, ASplitDoesNotChangeTheValueCurve)
    {
        const SyntheticMarket plain = testing::makeMarket(300, 4, 21);
        SyntheticMarket split = plain;
        split.addSplit(0, 120, 2.0);
        split.addSplit(2, 200, 0.5);
        split.addSplit(2, 201, 8.0);

        const ProportionalCost commission(0.0005);
        const EstimatedSpread plainSpread(plain, SpreadEstimator::CorwinSchultz);
        const EstimatedSpread splitSpread(split, SpreadEstimator::CorwinSchultz);
        const CostStack plainCosts({&commission, &plainSpread.cost});
        const CostStack splitCosts({&commission, &splitSpread.cost});
        for (const ExecutionTiming timing : {ExecutionTiming::NextOpen, ExecutionTiming::NextClose, ExecutionTiming::SameClose})
        {
            testing::Momentum s1(20, 5);
            testing::Momentum s2(20, 5);
            const BacktestResult a = run_backtest(plain.data(), s1, config(timing, &plainCosts));
            const BacktestResult b = run_backtest(split.data(), s2, config(timing, &splitCosts));
            for (Index t = 0; t < plain.days; ++t)
            {
                ASSERT_NEAR(b.value[t], a.value[t], 1e-9 * a.value[t]) << "day " << t;
                ASSERT_NEAR(b.cash[t], a.cash[t], 1e-9 * a.value[t]) << "day " << t;
            }
            // The shares did change: twice as many of asset 0 after its split.
            const Index last = plain.days - 1;
            EXPECT_NEAR(b.shares[plain.at(last, 0)], 2.0 * a.shares[plain.at(last, 0)], 1e-6 * std::abs(a.shares[plain.at(last, 0)]) + 1e-9);
            EXPECT_GT(a.journal.size(), 100u);
        }
    }

    // ---- Section 16: buy and hold ----

    TEST(Engine, BuyAndHoldTradesOnceAndKeepsItsShares)
    {
        SyntheticMarket m = eventfulMarket();
        m.cashRate.clear();
        testing::BuyAndHold strategy(equalWeights(m.assets, 0.9));
        const BacktestResult r = run_backtest(m.data(), strategy, config(ExecutionTiming::NextOpen));

        ASSERT_EQ(r.journal.size(), m.assets);
        std::vector<Real> shares(m.assets, 0.0);
        for (const OrderRecord &order : r.journal)
        {
            EXPECT_EQ(order.decision_date, m.dates[0]);
            EXPECT_EQ(order.execution_date, m.dates[1]);
            EXPECT_EQ(order.status, OrderStatus::Filled);
            EXPECT_DOUBLE_EQ(order.price, m.open[m.at(1, order.asset)]);
            shares[order.asset] = order.quantity;
        }

        // Final value recomputed outside the engine: the initial shares, as
        // multiplied by the splits, plus the cash left and every dividend.
        Real cash = Capital;
        for (Index i = 0; i < m.assets; ++i)
        {
            cash -= shares[i] * m.open[m.at(1, i)];
        }
        const MarketData data = m.data();
        const EventIndex events(data);
        for (Index t = 2; t < m.days; ++t)
        {
            for (Index i = 0; i < m.assets; ++i)
            {
                shares[i] *= events.split_ratio(t, i);
                cash += shares[i] * events.dividend(t, i);
            }
        }
        Real expected = cash;
        for (Index i = 0; i < m.assets; ++i)
        {
            expected += shares[i] * m.close[m.at(m.days - 1, i)];
            EXPECT_NEAR(r.shares[m.at(m.days - 1, i)], shares[i], 1e-9 * shares[i]);
        }
        EXPECT_NEAR(r.value.back(), expected, 1e-9 * expected);
        EXPECT_NEAR(r.cash[1], 0.1 * Capital, 1e-6);
    }

    // A dividend goes to whoever held the shares at the close before the
    // ex-date: selling at the open of the ex-date keeps it, buying then does
    // not earn it.
    TEST(Engine, TheDividendBelongsToTheHolderOfThePreviousClose)
    {
        SyntheticMarket m;
        m.days = 4;
        m.assets = 1;
        m.dates = {10959, 10960, 10961, 10962};
        m.open = {50.0, 50.0, 49.0, 49.0};
        m.close = {50.0, 50.0, 49.0, 49.0};
        m.dividends = {{0, 10961, 1.0}};
        {
            // Bought at the open of day 1, sold at the open of the ex-date.
            const std::vector<Real> targets = {1.0, 0.0};
            Scheduled strategy(targets, 1);
            const BacktestResult r = run_backtest(m.data(), strategy, config(ExecutionTiming::NextOpen));
            EXPECT_DOUBLE_EQ(r.dividends[2], Capital / 50.0);
            EXPECT_DOUBLE_EQ(r.shares[2], 0.0);
        }
        {
            // Bought at the open of the ex-date.
            const std::vector<Real> targets = {NaN, 1.0};
            Scheduled strategy(targets, 1);
            const BacktestResult r = run_backtest(m.data(), strategy, config(ExecutionTiming::NextOpen));
            EXPECT_GT(r.shares[2], 0.0);
            EXPECT_DOUBLE_EQ(r.dividends[2], 0.0);
        }
    }

    // ---- Section 16: the risk-free asset alone ----

    TEST(Engine, CashAloneEarnsTheRateAndNeverDrawsDown)
    {
        const SyntheticMarket m = testing::makeMarket(260, 3, 4);
        testing::Idle strategy;
        const BacktestResult r = run_backtest(m.data(), strategy, config());
        Real expected = Capital;
        EXPECT_DOUBLE_EQ(r.value[0], Capital);
        for (Index t = 1; t < m.days; ++t)
        {
            expected *= 1.0 + m.cashRate[t];
            ASSERT_NEAR(r.value[t], expected, 1e-9 * expected);
            ASSERT_GE(r.value[t], r.value[t - 1]);
            ASSERT_EQ(r.position_pnl[t], 0.0);
        }
        EXPECT_TRUE(r.journal.empty());
        // About 3 % over a year of 260 business days.
        EXPECT_NEAR(r.value.back() / Capital - 1.0, 0.03, 0.002);
    }

    // ---- Section 16: scale invariance ----

    TEST(Engine, DoublingTheCapitalDoublesEverything)
    {
        const SyntheticMarket m = eventfulMarket();
        const ProportionalCost cost(0.0005);
        testing::Momentum s1(20, 5);
        testing::Momentum s2(20, 5);
        BacktestConfig c = config(ExecutionTiming::NextOpen, &cost);
        const BacktestResult a = run_backtest(m.data(), s1, c);
        c.initial_capital = 2.0 * Capital;
        const BacktestResult b = run_backtest(m.data(), s2, c);
        for (Index t = 0; t < m.days; ++t)
        {
            ASSERT_NEAR(b.value[t], 2.0 * a.value[t], 1e-12 * a.value[t]);
            ASSERT_NEAR(b.costs[t], 2.0 * a.costs[t], 1e-12 * a.value[t]);
            ASSERT_NEAR(b.turnover[t], a.turnover[t], 1e-12);
        }
    }

    TEST(Engine, SameInputsSameBits)
    {
        const SyntheticMarket m = eventfulMarket();
        const EstimatedSpread spread(m, SpreadEstimator::CorwinSchultz);
        testing::Momentum s1(20, 5);
        testing::Momentum s2(20, 5);
        const BacktestResult a = run_backtest(m.data(), s1, config(ExecutionTiming::NextOpen, &spread.cost));
        const BacktestResult b = run_backtest(m.data(), s2, config(ExecutionTiming::NextOpen, &spread.cost));
        EXPECT_EQ(a.value, b.value);
        EXPECT_EQ(a.shares, b.shares);
        EXPECT_EQ(a.journal.size(), b.journal.size());
    }

    // ---- Finding 2: a missing price is never interpolated ----

    TEST(Engine, AMissingCloseKeepsTheLastPriceAndIsNotTradable)
    {
        SyntheticMarket m;
        m.days = 5;
        m.assets = 1;
        m.dates = {10959, 10960, 10961, 10962, 10963};
        m.close = {100.0, 100.0, NaN, 120.0, 120.0};

        // Bought at the close of day 1; day 2 has no quote.
        Once buy(0, DecisionKind::Orders, {10.0});
        const BacktestResult held = run_backtest(m.data(), buy, config());
        EXPECT_DOUBLE_EQ(held.value[1], Capital);
        // Not 110, the average of its neighbours: the last known price.
        EXPECT_DOUBLE_EQ(held.value[2], Capital);
        EXPECT_EQ(held.stale_positions[2], 1u);
        EXPECT_EQ(held.stale_positions[3], 0u);
        EXPECT_DOUBLE_EQ(held.value[3], Capital + 10.0 * 20.0);

        // The value on the gap day does not depend on the next price.
        SyntheticMarket later = m;
        later.close[3] = 500.0;
        Once buyAgain(0, DecisionKind::Orders, {10.0});
        EXPECT_EQ(run_backtest(later.data(), buyAgain, config()).value[2], held.value[2]);

        // An order due on the gap day is rejected, and is not executed later.
        Once late(1, DecisionKind::Orders, {10.0});
        const BacktestResult rejected = run_backtest(m.data(), late, config());
        ASSERT_EQ(rejected.journal.size(), 1u);
        EXPECT_EQ(rejected.journal[0].status, OrderStatus::RejectedNoQuote);
        EXPECT_EQ(rejected.journal[0].execution_date, m.dates[2]);
        EXPECT_EQ(rejected.journal[0].quantity, 0.0);
        EXPECT_TRUE(std::isnan(rejected.journal[0].price));
        EXPECT_DOUBLE_EQ(rejected.value.back(), Capital);
        EXPECT_EQ(rejected.quality.count(QualityCode::MissingQuotes), 1u);
    }

    // ---- Finding 8: signal and execution are not at the same close for free ----

    TEST(Engine, ExecutionComesAfterTheDecisionAtTheDeclaredPrice)
    {
        const SyntheticMarket m = testing::makeMarket(60, 3, 8);
        const std::vector<Real> weights = equalWeights(m.assets);
        const auto dayOf = [&](Date date)
        {
            return static_cast<Index>(std::lower_bound(m.dates.begin(), m.dates.end(), date) - m.dates.begin());
        };

        {
            testing::ConstantWeights strategy(weights, 10);
            const BacktestResult r = run_backtest(m.data(), strategy, config(ExecutionTiming::NextClose));
            ASSERT_FALSE(r.journal.empty());
            for (const OrderRecord &order : r.journal)
            {
                const Index decided = dayOf(order.decision_date);
                EXPECT_EQ(dayOf(order.execution_date), decided + 1);
                EXPECT_DOUBLE_EQ(order.price, m.close[m.at(decided + 1, order.asset)]);
            }
            EXPECT_NE(r.conventions.execution.find("close of t+1"), std::string::npos);
        }
        {
            testing::ConstantWeights strategy(weights, 10);
            BacktestConfig c = config(ExecutionTiming::NextOpen);
            c.extra_lag_days = 2;
            const BacktestResult r = run_backtest(m.data(), strategy, c);
            ASSERT_FALSE(r.journal.empty());
            for (const OrderRecord &order : r.journal)
            {
                const Index decided = dayOf(order.decision_date);
                EXPECT_EQ(dayOf(order.execution_date), decided + 3);
                EXPECT_DOUBLE_EQ(order.price, m.open[m.at(decided + 3, order.asset)]);
            }
            EXPECT_NE(r.conventions.execution.find("2 day(s) of extra delay"), std::string::npos);
        }
    }

    TEST(Engine, TradingAtTheSignalCloseNeedsAnExplicitCost)
    {
        const SyntheticMarket m = testing::makeMarket(60, 3, 8);
        testing::ConstantWeights free(equalWeights(m.assets), 10);
        EXPECT_THROW(run_backtest(m.data(), free, config(ExecutionTiming::SameClose)), Error);

        const ProportionalCost cost(0.001);
        testing::ConstantWeights paying(equalWeights(m.assets), 10);
        const BacktestResult r = run_backtest(m.data(), paying, config(ExecutionTiming::SameClose, &cost));
        ASSERT_FALSE(r.journal.empty());
        for (const OrderRecord &order : r.journal)
        {
            EXPECT_EQ(order.execution_date, order.decision_date);
            EXPECT_GT(order.cost, 0.0);
        }
        // Bought on day 0 at its close: the cost is the only change of value.
        EXPECT_NEAR(r.value[0], Capital / 1.001, 1e-6);
        EXPECT_NE(r.conventions.costs.find("10 basis points"), std::string::npos);
    }

    // ---- Costs inside the sizing ----

    TEST(Engine, TargetWeightsAreReachedNetOfCosts)
    {
        const SyntheticMarket m = testing::makeMarket(30, 4, 8, {.dividends = false, .annualRate = 0.0});
        const ProportionalCost cost(0.01);
        const std::vector<Real> weights = {0.4, 0.3, 0.2, 0.1};
        testing::BuyAndHold strategy(weights);
        const BacktestResult r = run_backtest(m.data(), strategy, config(ExecutionTiming::NextClose, &cost));
        const Real value = r.value[1];
        EXPECT_NEAR(value, Capital / 1.01, 1e-6);
        EXPECT_NEAR(r.cash[1], 0.0, 1e-6);
        for (Index i = 0; i < m.assets; ++i)
        {
            EXPECT_NEAR(r.shares[m.at(1, i)] * m.close[m.at(1, i)] / value, weights[i], 1e-12);
        }
        // From cash to fully invested: half of the amount traded over the value.
        EXPECT_NEAR(r.turnover[1], 0.5 * value / Capital, 1e-12);
    }

    TEST(Engine, OrdersWithoutACostEstimateAreRejected)
    {
        const SyntheticMarket m = testing::makeMarket(60, 2, 8);
        const EstimatedSpread spread(m, SpreadEstimator::CorwinSchultz);
        const SpreadCost &cost = spread.cost;
        testing::ConstantWeights strategy(equalWeights(m.assets), 10);
        const BacktestResult r = run_backtest(m.data(), strategy, config(ExecutionTiming::NextClose, &cost));
        for (const OrderRecord &order : r.journal)
        {
            const bool early = order.execution_date < m.dates[21];
            EXPECT_EQ(order.status, early ? OrderStatus::RejectedNoCostEstimate : OrderStatus::Filled);
        }
        EXPECT_DOUBLE_EQ(r.value[20], r.cash[20]);
        EXPECT_GT(r.traded_notional[21], 0.0);

        // The same run started after the warm-up trades from its first day.
        BacktestConfig c = config(ExecutionTiming::NextClose, &cost);
        c.start_index = 30;
        testing::ConstantWeights later(equalWeights(m.assets), 10);
        const BacktestResult warm = run_backtest(m.data(), later, c);
        ASSERT_EQ(warm.value.size(), m.days - 30);
        EXPECT_EQ(warm.dates.front(), m.dates[30]);
        EXPECT_DOUBLE_EQ(warm.value[0], Capital);
        ASSERT_FALSE(warm.journal.empty());
        EXPECT_EQ(warm.journal.front().status, OrderStatus::Filled);
        EXPECT_EQ(warm.journal.front().execution_date, m.dates[31]);
        expectConservation(m, warm, 30);
    }

    // ---- Cash ----

    TEST(Engine, ARejectedSaleDoesNotTurnIntoBorrowing)
    {
        SyntheticMarket m;
        m.days = 5;
        m.assets = 2;
        m.dates = {10959, 10960, 10961, 10962, 10963};
        m.close = {10.0, 20.0,
                   10.0, 20.0,
                   10.0, 20.0,
                   NaN, 20.0,
                   10.0, 20.0};
        // Fully in asset 0, then asked to switch to asset 1 on the day asset
        // 0 has no quote.
        const std::vector<Real> targets = {1.0, 0.0,
                                           NaN, NaN,
                                           0.0, 1.0};
        {
            Scheduled strategy(targets, m.assets);
            const BacktestResult r = run_backtest(m.data(), strategy, config());
            EXPECT_GE(r.cash[3], -1e-6);
            EXPECT_NEAR(r.shares[m.at(3, 1)], 0.0, 1e-6);
            EXPECT_DOUBLE_EQ(r.shares[m.at(3, 0)], Capital / 10.0);
            bool sawRejection = false;
            bool sawPartial = false;
            for (const OrderRecord &order : r.journal)
            {
                sawRejection |= order.status == OrderStatus::RejectedNoQuote && order.asset == 0;
                sawPartial |= order.status == OrderStatus::PartiallyFilled && order.asset == 1;
            }
            EXPECT_TRUE(sawRejection);
            EXPECT_TRUE(sawPartial);
        }
        {
            Scheduled strategy(targets, m.assets);
            BacktestConfig c = config();
            c.allow_borrowing = true;
            const BacktestResult r = run_backtest(m.data(), strategy, c);
            EXPECT_NEAR(r.cash[3], -Capital, 1e-6);
            EXPECT_NEAR(r.shares[m.at(3, 1)], Capital / 20.0, 1e-9);
        }
    }

    TEST(Engine, ANegativeBalancePaysTheDebitRate)
    {
        SyntheticMarket m = testing::makeMarket(20, 1, 8);
        std::vector<Real> debit(m.days, 0.001);
        MarketData data = m.data();
        data.debit_rate = debit;
        BacktestConfig c = config();
        c.allow_borrowing = true;
        testing::BuyAndHold strategy({2.0});
        const BacktestResult r = run_backtest(data, strategy, c);
        EXPECT_NEAR(r.cash[1], -Capital * (1.0 + m.cashRate[1]), 1e-6);
        EXPECT_NEAR(r.interest[2], 0.001 * r.cash[1], 1e-9);
        EXPECT_LT(r.interest[2], 0.0);
        expectConservation(m, r);
    }

    // ---- What the engine refuses ----

    TEST(Engine, RefusesShortsUniverseBreachesAndBadData)
    {
        SyntheticMarket m = testing::makeMarket(30, 2, 8);
        {
            testing::BuyAndHold strategy({-0.5, 0.5});
            EXPECT_THROW(run_backtest(m.data(), strategy, config()), Error);
        }
        {
            Once strategy(0, DecisionKind::Orders, {-1.0, 0.0});
            EXPECT_THROW(run_backtest(m.data(), strategy, config()), Error);
        }
        {
            SyntheticMarket restricted = m;
            restricted.universe.assign(m.days * m.assets, 1);
            restricted.universe[restricted.at(0, 1)] = 0;
            testing::BuyAndHold outside({0.5, 0.5});
            EXPECT_THROW(run_backtest(restricted.data(), outside, config()), Error);
            testing::BuyAndHold inside({0.5, 0.0});
            EXPECT_NO_THROW(run_backtest(restricted.data(), inside, config()));
        }
        {
            SyntheticMarket bad = m;
            bad.close[bad.at(3, 0)] = -1.0;
            testing::Idle strategy;
            try
            {
                run_backtest(bad.data(), strategy, config());
                FAIL() << "negative price accepted";
            }
            catch (const Error &error)
            {
                EXPECT_NE(std::string(error.what()).find("a price is zero, negative or infinite"), std::string::npos);
            }
        }
        {
            testing::Idle strategy;
            BacktestConfig c = config();
            c.initial_capital = 0.0;
            EXPECT_THROW(run_backtest(m.data(), strategy, c), Error);
            c = config();
            c.start_index = m.days;
            EXPECT_THROW(run_backtest(m.data(), strategy, c), Error);
            SyntheticMarket noOpen = m;
            noOpen.open.clear();
            EXPECT_THROW(run_backtest(noOpen.data(), strategy, config(ExecutionTiming::NextOpen)), Error);
        }
    }

    // A run on data checked beforehand gives the same book.
    TEST(Engine, SkippingTheQualityControlsChangesNoFigure)
    {
        const SyntheticMarket m = eventfulMarket();
        testing::Momentum s1(20, 5);
        testing::Momentum s2(20, 5);
        BacktestConfig c = config(ExecutionTiming::NextOpen);
        const BacktestResult full = run_backtest(m.data(), s1, c);
        c.data_already_checked = true;
        const BacktestResult fast = run_backtest(m.data(), s2, c);
        EXPECT_EQ(fast.value, full.value);
        EXPECT_EQ(fast.shares, full.shares);

        // Misshapen data is still refused.
        MarketData data = m.data();
        data.close = {m.close.data(), m.days - 1, m.assets};
        testing::Idle idle;
        EXPECT_THROW(run_backtest(data, idle, c), Error);
    }

    // ---- Progress and cancellation ----

    TEST(Engine, ReportsProgressAndStopsWhenCancelled)
    {
        const SyntheticMarket m = testing::makeMarket(500, 2, 8);
        std::vector<double> seen;
        RunControl control;
        control.progress = [&](double fraction)
        { seen.push_back(fraction); };
        testing::Idle strategy;
        run_backtest(m.data(), strategy, config(), control);
        ASSERT_FALSE(seen.empty());
        EXPECT_TRUE(std::is_sorted(seen.begin(), seen.end()));
        EXPECT_DOUBLE_EQ(seen.back(), 1.0);
        EXPECT_LE(seen.size(), 102u);

        std::atomic<bool> cancel{false};
        control.cancel = &cancel;
        control.progress = [&](double fraction)
        { cancel = fraction >= 0.5; };
        EXPECT_THROW(run_backtest(m.data(), strategy, config(), control), Cancelled);
    }

} // namespace quantBacktest
