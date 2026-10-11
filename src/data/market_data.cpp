#include "quantBacktest/data/market_data.hpp"

#include "quantBacktest/core/error.hpp"

#include <algorithm>
#include <functional>
#include <string>

namespace quantBacktest
{

    namespace
    {

        // Stable counting sort of the events by the day they take effect.
        template <typename Event, typename DateOf>
        void indexEvents(std::span<const Event> events, std::span<const Date> dates, DateOf dateOf,
                         std::vector<Event> &sorted, std::vector<Index> &offsets)
        {
            const Index days = dates.size();
            std::vector<Index> dayOf(events.size(), npos);
            offsets.assign(days + 1, 0);
            for (Index k = 0; k < events.size(); ++k)
            {
                const Date date = dateOf(events[k]);
                if (days == 0 || date < dates.front() || date > dates.back())
                {
                    continue;
                }
                const auto it = std::lower_bound(dates.begin(), dates.end(), date);
                dayOf[k] = static_cast<Index>(it - dates.begin());
                ++offsets[dayOf[k] + 1];
            }
            for (Index t = 0; t < days; ++t)
            {
                offsets[t + 1] += offsets[t];
            }
            sorted.resize(offsets[days]);
            std::vector<Index> next(offsets.begin(), offsets.end() - 1);
            for (Index k = 0; k < events.size(); ++k)
            {
                if (dayOf[k] != npos)
                {
                    sorted[next[dayOf[k]]++] = events[k];
                }
            }
        }

    } // namespace

    EventIndex::EventIndex(const MarketData &market)
    {
        if (std::adjacent_find(market.dates.begin(), market.dates.end(), std::greater_equal<>()) != market.dates.end())
        {
            throw Error("dates must be strictly increasing");
        }
        const auto outOfRange = [&](const auto &event)
        { return event.asset >= market.assets; };
        if (std::any_of(market.dividends.begin(), market.dividends.end(), outOfRange) ||
            std::any_of(market.splits.begin(), market.splits.end(), outOfRange))
        {
            throw Error("a dividend or a split refers to an asset that does not exist");
        }
        indexEvents(
            market.dividends, market.dates, [](const Dividend &d)
            { return d.ex_date; },
            dividends_, dividend_offsets_);
        indexEvents(
            market.splits, market.dates, [](const Split &s)
            { return s.date; },
            splits_, split_offsets_);
    }

    std::span<const Dividend> EventIndex::dividends_on(Index day) const noexcept
    {
        return {dividends_.data() + dividend_offsets_[day], dividend_offsets_[day + 1] - dividend_offsets_[day]};
    }

    std::span<const Split> EventIndex::splits_on(Index day) const noexcept
    {
        return {splits_.data() + split_offsets_[day], split_offsets_[day + 1] - split_offsets_[day]};
    }

    std::span<const Dividend> EventIndex::dividends_through(Index day) const noexcept
    {
        return {dividends_.data(), dividend_offsets_[day + 1]};
    }

    std::span<const Split> EventIndex::splits_through(Index day) const noexcept
    {
        return {splits_.data(), split_offsets_[day + 1]};
    }

    Real EventIndex::split_ratio(Index day, Index asset) const noexcept
    {
        Real ratio = 1.0;
        for (const Split &split : splits_on(day))
        {
            if (split.asset == asset)
            {
                ratio *= split.ratio;
            }
        }
        return ratio;
    }

    Real EventIndex::dividend(Index day, Index asset) const noexcept
    {
        Real amount = 0.0;
        for (const Dividend &dividend : dividends_on(day))
        {
            if (dividend.asset == asset)
            {
                amount += dividend.amount;
            }
        }
        return amount;
    }

    MarketView::MarketView(const MarketData &market, const EventIndex &events, Index day)
        : day_(day),
          assets_(market.assets),
          dates_(market.dates.first(day + 1)),
          close_(market.close.first_rows(day + 1)),
          open_(market.open.first_rows(day + 1)),
          high_(market.high.first_rows(day + 1)),
          low_(market.low.first_rows(day + 1)),
          volume_(market.volume.first_rows(day + 1)),
          universe_(market.universe.empty() ? market.universe : market.universe.first((day + 1) * market.assets)),
          events_(&events)
    {
    }

    bool MarketView::in_universe(Index s, Index asset) const
    {
        const Index row = checked(s);
        return universe_.empty() || universe_[row * assets_ + asset] != 0;
    }

    Index MarketView::checked(Index s) const
    {
        if (s > day_)
        {
            throw Error("look-ahead: day " + std::to_string(s) + " was read from a view that ends on day " + std::to_string(day_));
        }
        return s;
    }

    const MatrixView &MarketView::required(const MatrixView &table, const char *name)
    {
        if (table.empty())
        {
            throw Error(std::string("the '") + name + "' table was not given");
        }
        return table;
    }

} // namespace quantBacktest
