#include "quantBacktest/data/returns.hpp"

#include "quantBacktest/core/error.hpp"

#include <limits>

namespace quantBacktest
{

    std::vector<Real> total_returns(const MarketData &market)
    {
        const Index days = market.dates.size();
        const Index assets = market.assets;
        if (market.close.rows() != days || market.close.cols() != assets)
        {
            throw Error("the 'close' table does not have one row per date and one column per asset");
        }
        const Real nan = std::numeric_limits<Real>::quiet_NaN();
        std::vector<Real> returns(days * assets, nan);
        // Dense factors for the day, reset after use: events are sparse.
        std::vector<Real> ratio(assets, 1.0);
        std::vector<Real> dividend(assets, 0.0);
        const EventIndex events(market);
        for (Index t = 1; t < days; ++t)
        {
            for (const Split &split : events.splits_on(t))
            {
                ratio[split.asset] *= split.ratio;
            }
            for (const Dividend &d : events.dividends_on(t))
            {
                dividend[d.asset] += d.amount;
            }
            for (Index i = 0; i < assets; ++i)
            {
                const Real previous = market.close(t - 1, i);
                const Real current = market.close(t, i);
                if (!is_missing(previous) && !is_missing(current))
                {
                    returns[t * assets + i] = ratio[i] * (current + dividend[i]) / previous - 1.0;
                }
            }
            for (const Split &split : events.splits_on(t))
            {
                ratio[split.asset] = 1.0;
            }
            for (const Dividend &d : events.dividends_on(t))
            {
                dividend[d.asset] = 0.0;
            }
        }
        return returns;
    }

    std::vector<Real> period_rates_from_annualized(std::span<const Date> dates, std::span<const Real> annual, Real basis)
    {
        if (annual.size() != dates.size())
        {
            throw Error("the rate series does not have one value per date");
        }
        if (!(basis > 0.0))
        {
            throw Error("the day-count basis must be positive");
        }
        std::vector<Real> rates(dates.size(), 0.0);
        for (Index t = 1; t < dates.size(); ++t)
        {
            rates[t] = annual[t - 1] * static_cast<Real>(dates[t] - dates[t - 1]) / basis;
        }
        return rates;
    }

} // namespace quantBacktest
