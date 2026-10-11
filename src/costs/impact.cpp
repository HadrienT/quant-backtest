#include "quantBacktest/costs/impact.hpp"

#include "quantBacktest/core/error.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace quantBacktest
{

    namespace
    {

        constexpr Real kNaN = std::numeric_limits<Real>::quiet_NaN();

        void requireWindow(Index window, Index min_observations)
        {
            if (window < 2 || min_observations < 2 || min_observations > window)
            {
                throw Error("the window must hold at least 2 days, and the minimum number of observations must be between 2 and the window");
            }
        }

        bool shaped(const MarketData &market, const MatrixView &table)
        {
            return !table.empty() && table.rows() == market.dates.size() && table.cols() == market.assets;
        }

    } // namespace

    std::vector<Real> trailing_volatility(const MarketData &market, Index window, Index min_observations)
    {
        requireWindow(window, min_observations);
        if (!shaped(market, market.close))
        {
            throw Error("the 'close' table must have one row per date and one column per asset");
        }
        const Index days = market.dates.size();
        const Index assets = market.assets;
        const EventIndex events(market);

        // Row t: return of day t, splits undone; NaN across a missing quote.
        std::vector<Real> returns(days * assets, kNaN);
        std::vector<Real> ratio(assets, 1.0);
        for (Index t = 1; t < days; ++t)
        {
            for (const Split &split : events.splits_on(t))
            {
                ratio[split.asset] *= split.ratio;
            }
            for (Index i = 0; i < assets; ++i)
            {
                const Real previous = market.close(t - 1, i);
                const Real current = market.close(t, i);
                if (!is_missing(previous) && !is_missing(current))
                {
                    returns[t * assets + i] = ratio[i] * current / previous - 1.0;
                }
            }
            for (const Split &split : events.splits_on(t))
            {
                ratio[split.asset] = 1.0;
            }
        }

        std::vector<Real> out(days * assets, kNaN);
        std::vector<Real> sum(assets);
        std::vector<Real> squares(assets);
        std::vector<Index> count(assets);
        for (Index t = window; t < days; ++t)
        {
            std::fill(sum.begin(), sum.end(), 0.0);
            std::fill(squares.begin(), squares.end(), 0.0);
            std::fill(count.begin(), count.end(), Index{0});
            for (Index s = t - window; s < t; ++s)
            {
                const Real *row = returns.data() + s * assets;
                for (Index i = 0; i < assets; ++i)
                {
                    if (!is_missing(row[i]))
                    {
                        sum[i] += row[i];
                        squares[i] += row[i] * row[i];
                        ++count[i];
                    }
                }
            }
            for (Index i = 0; i < assets; ++i)
            {
                if (count[i] >= min_observations)
                {
                    const Real n = static_cast<Real>(count[i]);
                    const Real mean = sum[i] / n;
                    out[t * assets + i] = std::sqrt(std::max(squares[i] - n * mean * mean, 0.0) / (n - 1.0));
                }
            }
        }
        return out;
    }

    std::vector<Real> trailing_dollar_volume(const MarketData &market, Index window, Index min_observations)
    {
        requireWindow(window, min_observations);
        if (!shaped(market, market.close) || !shaped(market, market.volume))
        {
            throw Error("the traded amount needs the 'close' and 'volume' tables, one row per date and one column per asset");
        }
        const Index days = market.dates.size();
        const Index assets = market.assets;
        std::vector<Real> out(days * assets, kNaN);
        std::vector<Real> sum(assets);
        std::vector<Index> count(assets);
        for (Index t = window; t < days; ++t)
        {
            std::fill(sum.begin(), sum.end(), 0.0);
            std::fill(count.begin(), count.end(), Index{0});
            for (Index s = t - window; s < t; ++s)
            {
                for (Index i = 0; i < assets; ++i)
                {
                    const Real amount = market.close(s, i) * market.volume(s, i);
                    if (!is_missing(amount))
                    {
                        sum[i] += amount;
                        ++count[i];
                    }
                }
            }
            for (Index i = 0; i < assets; ++i)
            {
                if (count[i] >= min_observations)
                {
                    out[t * assets + i] = sum[i] / static_cast<Real>(count[i]);
                }
            }
        }
        return out;
    }

    SquareRootImpactCost::SquareRootImpactCost(MatrixView volatility, MatrixView dollar_volume, Real coefficient)
        : volatility_(volatility), dollar_volume_(dollar_volume), coefficient_(coefficient)
    {
        if (volatility.empty() || dollar_volume.empty())
        {
            throw Error("the impact cost needs a table of volatilities and a table of traded amounts");
        }
        if (!(std::isfinite(coefficient) && coefficient >= 0.0))
        {
            throw Error("the impact coefficient must be a non-negative number");
        }
    }

    void SquareRootImpactCost::validate(const MarketData &market) const
    {
        if (!shaped(market, volatility_) || !shaped(market, dollar_volume_))
        {
            throw Error("the tables of the impact cost must have one row per date and one column per asset");
        }
    }

    Real SquareRootImpactCost::cost(const MarketView &, const Fill &fill) const
    {
        const Real volatility = volatility_(fill.day, fill.asset);
        const Real volume = dollar_volume_(fill.day, fill.asset);
        if (!(volatility > 0.0) || !(volume > 0.0))
        {
            return kNaN;
        }
        const Real amount = std::abs(fill.quantity) * fill.price;
        return coefficient_ * volatility * std::sqrt(amount / volume) * amount;
    }

    std::string SquareRootImpactCost::describe() const
    {
        char buffer[160];
        std::snprintf(buffer, sizeof(buffer),
                      "square-root market impact: %g x daily volatility x sqrt(amount traded / daily traded amount), on the amount traded", coefficient_);
        return buffer;
    }

    Participation participation(const BacktestResult &result, MatrixView dollar_volume, std::span<const Date> dates)
    {
        if (dollar_volume.empty() || dollar_volume.rows() != dates.size())
        {
            throw Error("the table of traded amounts must have one row per date of the market");
        }
        std::vector<Real> shares;
        for (const OrderRecord &order : result.journal)
        {
            if (order.quantity == 0.0 || order.asset >= dollar_volume.cols())
            {
                continue;
            }
            const auto it = std::lower_bound(dates.begin(), dates.end(), order.execution_date);
            if (it == dates.end() || *it != order.execution_date)
            {
                continue;
            }
            const Real volume = dollar_volume(static_cast<Index>(it - dates.begin()), order.asset);
            if (volume > 0.0)
            {
                shares.push_back(std::abs(order.quantity) * order.price / volume);
            }
        }
        Participation out;
        out.executions = shares.size();
        if (shares.empty())
        {
            return out;
        }
        std::sort(shares.begin(), shares.end());
        out.maximum = shares.back();
        const Index middle = shares.size() / 2;
        out.median = shares.size() % 2 == 1 ? shares[middle] : 0.5 * (shares[middle - 1] + shares[middle]);
        return out;
    }

} // namespace quantBacktest
