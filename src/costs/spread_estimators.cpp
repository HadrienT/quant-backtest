#include "quantBacktest/costs/spread_estimators.hpp"

#include "quantBacktest/core/error.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace quantBacktest
{

    namespace
    {

        constexpr Real kNaN = std::numeric_limits<Real>::quiet_NaN();

        void requireSameLength(std::span<const Real> high, std::span<const Real> low, std::span<const Real> close)
        {
            if (high.size() != low.size() || high.size() != close.size())
            {
                throw Error("high, low and close must have the same length");
            }
        }

        bool anyMissing(std::span<const Real> high, std::span<const Real> low, std::span<const Real> close, Index t)
        {
            return is_missing(high[t]) || is_missing(low[t]) || is_missing(close[t]) ||
                   is_missing(high[t + 1]) || is_missing(low[t + 1]) || is_missing(close[t + 1]);
        }

        Real square(Real x)
        {
            return x * x;
        }

        // The two-day estimate of Corwin and Schultz, floored at zero. Both
        // days are in the same share count.
        Real corwinSchultzPair(Real high, Real low, Real close, Real nextHigh, Real nextLow)
        {
            static const Real k = 3.0 - 2.0 * std::sqrt(2.0);
            if (nextLow > close)
            {
                const Real gap = nextLow - close;
                nextHigh -= gap;
                nextLow -= gap;
            }
            else if (nextHigh < close)
            {
                const Real gap = close - nextHigh;
                nextHigh += gap;
                nextLow += gap;
            }
            const Real beta = square(std::log(high / low)) + square(std::log(nextHigh / nextLow));
            const Real gamma = square(std::log(std::max(high, nextHigh) / std::min(low, nextLow)));
            const Real alpha = (std::sqrt(2.0 * beta) - std::sqrt(beta)) / k - std::sqrt(gamma / k);
            const Real spread = 2.0 * std::expm1(alpha) / (1.0 + std::exp(alpha));
            return std::max(spread, 0.0);
        }

        // One term of the covariance of Abdi and Ranaldo.
        Real abdiRanaldoPair(Real high, Real low, Real close, Real nextHigh, Real nextLow)
        {
            const Real c = std::log(close);
            const Real eta = 0.5 * (std::log(high) + std::log(low));
            const Real nextEta = 0.5 * (std::log(nextHigh) + std::log(nextLow));
            return (c - eta) * (c - nextEta);
        }

        Real corwinSchultzFromSum(Real sum, Index pairs)
        {
            return sum / static_cast<Real>(pairs);
        }

        Real abdiRanaldoFromSum(Real sum, Index pairs)
        {
            return std::sqrt(std::max(4.0 * sum / static_cast<Real>(pairs), 0.0));
        }

    } // namespace

    Real corwin_schultz_spread(std::span<const Real> high, std::span<const Real> low, std::span<const Real> close, Index min_pairs)
    {
        requireSameLength(high, low, close);
        Real sum = 0.0;
        Index pairs = 0;
        for (Index t = 0; t + 1 < high.size(); ++t)
        {
            if (!anyMissing(high, low, close, t))
            {
                sum += corwinSchultzPair(high[t], low[t], close[t], high[t + 1], low[t + 1]);
                ++pairs;
            }
        }
        return pairs == 0 || pairs < min_pairs ? kNaN : corwinSchultzFromSum(sum, pairs);
    }

    Real abdi_ranaldo_spread(std::span<const Real> high, std::span<const Real> low, std::span<const Real> close, Index min_pairs)
    {
        requireSameLength(high, low, close);
        Real sum = 0.0;
        Index pairs = 0;
        for (Index t = 0; t + 1 < high.size(); ++t)
        {
            if (!anyMissing(high, low, close, t))
            {
                sum += abdiRanaldoPair(high[t], low[t], close[t], high[t + 1], low[t + 1]);
                ++pairs;
            }
        }
        return pairs == 0 || pairs < min_pairs ? kNaN : abdiRanaldoFromSum(sum, pairs);
    }

    std::vector<Real> estimated_spreads(const MarketData &market, SpreadEstimator estimator, Index window, Index min_pairs)
    {
        const Index days = market.dates.size();
        const Index assets = market.assets;
        const auto shaped = [&](const MatrixView &table)
        { return !table.empty() && table.rows() == days && table.cols() == assets; };
        if (!shaped(market.high) || !shaped(market.low) || !shaped(market.close))
        {
            throw Error("estimating spreads needs the 'high', 'low' and 'close' tables, one row per date and one column per asset");
        }
        if (window < 2)
        {
            throw Error("the spread estimation window must hold at least two days");
        }
        if (min_pairs < 1 || min_pairs > window - 1)
        {
            throw Error("the minimum number of two-day pairs must be between 1 and the window minus one");
        }
        const bool corwinSchultz = estimator == SpreadEstimator::CorwinSchultz;
        const EventIndex events(market);

        // Row t: the two-day term of the pair (t-1, t), day t brought back to
        // the share count of t-1.
        std::vector<Real> pair(days * assets, kNaN);
        std::vector<Real> ratio(assets, 1.0);
        for (Index t = 1; t < days; ++t)
        {
            for (const Split &split : events.splits_on(t))
            {
                ratio[split.asset] *= split.ratio;
            }
            for (Index i = 0; i < assets; ++i)
            {
                const Real high = market.high(t - 1, i);
                const Real low = market.low(t - 1, i);
                const Real close = market.close(t - 1, i);
                const Real nextHigh = market.high(t, i) * ratio[i];
                const Real nextLow = market.low(t, i) * ratio[i];
                if (is_missing(high) || is_missing(low) || is_missing(close) || is_missing(nextHigh) || is_missing(nextLow) || is_missing(market.close(t, i)))
                {
                    continue;
                }
                pair[t * assets + i] = corwinSchultz ? corwinSchultzPair(high, low, close, nextHigh, nextLow)
                                                     : abdiRanaldoPair(high, low, close, nextHigh, nextLow);
            }
            for (const Split &split : events.splits_on(t))
            {
                ratio[split.asset] = 1.0;
            }
        }

        // Row t: the pairs inside [t - window, t - 1], summed in date order.
        std::vector<Real> spreads(days * assets, kNaN);
        std::vector<Real> sum(assets);
        std::vector<Index> count(assets);
        for (Index t = window; t < days; ++t)
        {
            std::fill(sum.begin(), sum.end(), 0.0);
            std::fill(count.begin(), count.end(), Index{0});
            for (Index s = t - window + 1; s < t; ++s)
            {
                const Real *row = pair.data() + s * assets;
                for (Index i = 0; i < assets; ++i)
                {
                    if (!is_missing(row[i]))
                    {
                        sum[i] += row[i];
                        ++count[i];
                    }
                }
            }
            for (Index i = 0; i < assets; ++i)
            {
                if (count[i] >= min_pairs)
                {
                    spreads[t * assets + i] = corwinSchultz ? corwinSchultzFromSum(sum[i], count[i]) : abdiRanaldoFromSum(sum[i], count[i]);
                }
            }
        }
        return spreads;
    }

} // namespace quantBacktest
