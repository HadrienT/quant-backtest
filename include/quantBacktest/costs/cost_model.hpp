#ifndef QUANTBACKTEST_COSTS_COST_MODEL_HPP
#define QUANTBACKTEST_COSTS_COST_MODEL_HPP

#include "quantBacktest/core/types.hpp"
#include "quantBacktest/data/market_data.hpp"

#include <string>
#include <vector>

namespace quantBacktest
{

    /// An execution to be costed: `quantity` shares (positive to buy) of
    /// `asset` at `price`, on day `day` of the calendar.
    struct Fill
    {
        Index day = 0;
        Index asset = 0;
        Real quantity = 0.0;
        Real price = 0.0;
    };

    /// A transaction cost (companion, section 5.1), paid in cash on top of
    /// the execution price.
    class CostModel
    {
      public:
        virtual ~CostModel() = default;

        /// Cost of the fill in currency, never negative. The view ends on the
        /// day of the fill. NaN means the cost cannot be estimated from what
        /// is known: the engine then rejects the order instead of trading for
        /// free.
        virtual Real cost(const MarketView &market, const Fill &fill) const = 0;

        /// Throws `Error` if the data lack what the model needs.
        virtual void validate(const MarketData &market) const;

        /// The convention, in English, written under the result.
        virtual std::string describe() const = 0;
    };

    /// Commission, fees and taxes proportional to the amount traded:
    /// `rate` times |quantity| times price.
    class ProportionalCost final : public CostModel
    {
      public:
        explicit ProportionalCost(Real rate);
        Real cost(const MarketView &market, const Fill &fill) const override;
        std::string describe() const override;

      private:
        Real rate_;
    };

    /// Half the bid-ask spread on the amount traded: 0.5 x spread x |quantity|
    /// x price. The spreads are a table with one row per date and one column
    /// per asset, read on the day of the fill: quoted spreads if the caller
    /// has them, or `estimated_spreads` (costs/spread_estimators.hpp). Each
    /// row must have been known before trading that day. A missing spread
    /// makes the cost NaN, and the order is rejected.
    class SpreadCost final : public CostModel
    {
      public:
        /// The table is not owned. `source` says where the spreads come from;
        /// it is written in the conventions of the result.
        SpreadCost(MatrixView spreads, std::string source);
        Real cost(const MarketView &market, const Fill &fill) const override;
        void validate(const MarketData &market) const override;
        std::string describe() const override;

      private:
        MatrixView spreads_;
        std::string source_;
    };

    /// The sum of several costs. The models are not owned and must outlive
    /// the stack.
    class CostStack final : public CostModel
    {
      public:
        explicit CostStack(std::vector<const CostModel *> models);
        Real cost(const MarketView &market, const Fill &fill) const override;
        void validate(const MarketData &market) const override;
        std::string describe() const override;

      private:
        std::vector<const CostModel *> models_;
    };

} // namespace quantBacktest

#endif
