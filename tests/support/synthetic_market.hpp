#ifndef QUANTBACKTEST_TESTS_SUPPORT_SYNTHETIC_MARKET_HPP
#define QUANTBACKTEST_TESTS_SUPPORT_SYNTHETIC_MARKET_HPP

#include "quantBacktest/data/market_data.hpp"
#include "quantBacktest/engine/strategy.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

namespace quantBacktest::testing
{

    /// Owns the arrays a test hands to the engine. The engine is tested on
    /// data built here, never on a database (blueprint, section 3).
    struct SyntheticMarket
    {
        Index days = 0;
        Index assets = 0;
        std::vector<Date> dates;
        std::vector<Real> open;
        std::vector<Real> high;
        std::vector<Real> low;
        std::vector<Real> close;
        std::vector<Real> volume;
        std::vector<Real> cashRate;
        std::vector<std::uint8_t> universe;
        std::vector<Dividend> dividends;
        std::vector<Split> splits;

        MarketData data() const
        {
            MarketData m;
            m.dates = dates;
            m.assets = assets;
            m.close = {close.data(), days, assets};
            if (!open.empty())
            {
                m.open = {open.data(), days, assets};
            }
            if (!high.empty())
            {
                m.high = {high.data(), days, assets};
                m.low = {low.data(), days, assets};
            }
            if (!volume.empty())
            {
                m.volume = {volume.data(), days, assets};
            }
            m.cash_rate = cashRate;
            m.universe = universe;
            m.dividends = dividends;
            m.splits = splits;
            return m;
        }

        Index at(Index day, Index asset) const { return day * assets + asset; }

        /// Rewrites the raw data as if `asset` had split on `day`: nothing
        /// changes for a shareholder.
        void addSplit(Index asset, Index day, Real ratio)
        {
            for (Index t = day; t < days; ++t)
            {
                for (std::vector<Real> *table : {&open, &high, &low, &close})
                {
                    if (!table->empty())
                    {
                        (*table)[at(t, asset)] /= ratio;
                    }
                }
                if (!volume.empty())
                {
                    volume[at(t, asset)] *= ratio;
                }
            }
            for (Dividend &d : dividends)
            {
                if (d.asset == asset && d.ex_date >= dates[day])
                {
                    d.amount /= ratio;
                }
            }
            splits.push_back({asset, dates[day], ratio});
        }
    };

    struct MarketOptions
    {
        bool dividends = true;
        Real annualRate = 0.03;
    };

    /// Business days from Monday 2000-01-03, lognormal daily bars, quarterly
    /// dividends. Deterministic for a given seed.
    inline SyntheticMarket makeMarket(Index days, Index assets, std::uint64_t seed, const MarketOptions &options = {})
    {
        SyntheticMarket m;
        m.days = days;
        m.assets = assets;
        Date date = 10959;
        for (Index t = 0; t < days; ++t)
        {
            m.dates.push_back(date);
            date += (date - 10959) % 7 == 4 ? 3 : 1;
        }
        std::mt19937_64 rng(seed);
        std::normal_distribution<Real> normal(0.0, 1.0);
        std::uniform_real_distribution<Real> uniform(0.0, 1.0);
        const Index size = days * assets;
        m.open.resize(size);
        m.high.resize(size);
        m.low.resize(size);
        m.close.resize(size);
        m.volume.resize(size);
        for (Index i = 0; i < assets; ++i)
        {
            const Real volatility = 0.01 + 0.02 * uniform(rng);
            const Real drift = 0.0003;
            Real price = 20.0 + 180.0 * uniform(rng);
            for (Index t = 0; t < days; ++t)
            {
                const Real open = price * std::exp(0.3 * volatility * normal(rng));
                const Real close = open * std::exp(drift + volatility * normal(rng));
                m.open[m.at(t, i)] = open;
                m.close[m.at(t, i)] = close;
                m.high[m.at(t, i)] = std::max(open, close) * (1.0 + 0.5 * volatility * uniform(rng));
                m.low[m.at(t, i)] = std::min(open, close) * (1.0 - 0.5 * volatility * uniform(rng));
                m.volume[m.at(t, i)] = 1e5 + 9e5 * uniform(rng);
                if (options.dividends && t > 0 && (t + 7 * i) % 63 == 0)
                {
                    m.dividends.push_back({i, m.dates[t], 0.005 * close});
                }
                price = close;
            }
        }
        if (options.annualRate != 0.0)
        {
            m.cashRate.assign(days, 0.0);
            for (Index t = 1; t < days; ++t)
            {
                m.cashRate[t] = options.annualRate * static_cast<Real>(m.dates[t] - m.dates[t - 1]) / 360.0;
            }
        }
        return m;
    }

    /// Targets fixed weights once, on its first call, then does nothing.
    class BuyAndHold final : public Strategy
    {
      public:
        explicit BuyAndHold(std::vector<Real> weights)
            : weights_(std::move(weights)) {}

        void decide(const MarketView &, const BookView &, Decision &decision) override
        {
            if (!done_)
            {
                decision.kind = DecisionKind::TargetWeights;
                decision.values = weights_;
                done_ = true;
            }
        }

      private:
        std::vector<Real> weights_;
        bool done_ = false;
    };

    /// Targets the same weights every `period` days.
    class ConstantWeights final : public Strategy
    {
      public:
        ConstantWeights(std::vector<Real> weights, Index period)
            : weights_(std::move(weights)), period_(period) {}

        void decide(const MarketView &market, const BookView &, Decision &decision) override
        {
            if (market.day() % period_ == 0)
            {
                decision.kind = DecisionKind::TargetWeights;
                decision.values = weights_;
            }
        }

      private:
        std::vector<Real> weights_;
        Index period_;
    };

    /// Every `period` days, equal weights on the assets whose close rose over
    /// the last `lookback` days, splits undone. It reads the past through the
    /// view, as a real strategy does.
    class Momentum final : public Strategy
    {
      public:
        Momentum(Index lookback, Index period)
            : lookback_(lookback), period_(period) {}

        void decide(const MarketView &market, const BookView &, Decision &decision) override
        {
            const Index t = market.day();
            if (t < lookback_ || t % period_ != 0)
            {
                return;
            }
            Index winners = 0;
            for (Index i = 0; i < market.assets(); ++i)
            {
                Real factor = 1.0;
                for (Index s = t - lookback_ + 1; s <= t; ++s)
                {
                    factor *= market.split_ratio(s, i);
                }
                const bool winner = market.close(t, i) * factor > market.close(t - lookback_, i);
                decision.values[i] = winner ? 1.0 : 0.0;
                winners += winner ? 1 : 0;
            }
            decision.kind = DecisionKind::TargetWeights;
            for (Real &w : decision.values)
            {
                w = winners == 0 ? 0.0 : w / static_cast<Real>(winners);
            }
        }

      private:
        Index lookback_;
        Index period_;
    };

    /// Does nothing: the book stays in cash.
    class Idle final : public Strategy
    {
      public:
        void decide(const MarketView &, const BookView &, Decision &) override {}
    };

} // namespace quantBacktest::testing

#endif
