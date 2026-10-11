#ifndef QUANTBACKTEST_ENGINE_STRATEGY_HPP
#define QUANTBACKTEST_ENGINE_STRATEGY_HPP

#include "quantBacktest/core/types.hpp"
#include "quantBacktest/data/market_data.hpp"

#include <span>
#include <vector>

namespace quantBacktest
{

    /// The book as the strategy sees it when it decides: after the close of
    /// the decision day, positions valued at their last known price.
    struct BookView
    {
        Real cash = 0.0;
        Real value = 0.0;
        /// Number of shares held, per asset.
        std::span<const Real> shares;
        /// Last known price per asset, in today's share count; NaN if the
        /// asset has never been quoted.
        std::span<const Real> prices;
    };

    enum class DecisionKind
    {
        /// Nothing to do today.
        None,
        /// `values[i]` is the fraction of the portfolio value to hold in
        /// asset i once the orders are executed; what is left is cash. NaN
        /// leaves the asset untouched.
        TargetWeights,
        /// `values[i]` is a number of shares to buy (positive) or sell
        /// (negative). NaN or zero is no order.
        Orders,
    };

    struct Decision
    {
        DecisionKind kind = DecisionKind::None;
        /// One value per asset. The engine hands it over filled with NaN.
        std::vector<Real> values;
    };

    /// A strategy is a function of what was known on the decision day
    /// (companion, section 14). The engine calls it once per day, after the
    /// close, and knows nothing else about it.
    class Strategy
    {
      public:
        virtual ~Strategy() = default;

        /// `market` ends on the decision day: reading a later row throws.
        virtual void decide(const MarketView &market, const BookView &book, Decision &decision) = 0;
    };

} // namespace quantBacktest

#endif
