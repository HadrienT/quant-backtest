#ifndef QUANTBACKTEST_COSTS_IMPACT_HPP
#define QUANTBACKTEST_COSTS_IMPACT_HPP

#include "quantBacktest/core/types.hpp"
#include "quantBacktest/costs/cost_model.hpp"
#include "quantBacktest/data/market_data.hpp"
#include "quantBacktest/engine/backtest.hpp"

#include <vector>

namespace quantBacktest
{

    // Market impact (companion, section 5.1): what limits the capacity of a
    // strategy. The two tables below are what the model needs to know about
    // each asset before trading on a day; like the estimated spreads, row t
    // only reads the days before t.

    /// Standard deviation of the daily close-to-close returns (splits undone)
    /// over the `window` days that precede t; one row per date, row-major.
    /// NaN with fewer than `min_observations` returns.
    std::vector<Real> trailing_volatility(const MarketData &market, Index window, Index min_observations);

    /// Mean of close times volume over the `window` days that precede t: the
    /// amount traded per day, in currency, which a split does not change.
    /// Needs the 'volume' table. NaN with fewer than `min_observations` days.
    std::vector<Real> trailing_dollar_volume(const MarketData &market, Index window, Index min_observations);

    /// Square-root impact: an order of amount A in an asset of daily
    /// volatility s and daily traded amount V moves the price, on average, by
    ///
    ///     Y s sqrt(A / V)
    ///
    /// and costs that times A, so the cost grows as the power 3/2 of the size
    /// (Almgren, Thum, Hauptmann and Li, "Direct Estimation of Equity Market
    /// Impact", Risk, 2005; Toth et al. 2011). Y is of the order of 1.
    ///
    /// The tables are not owned. A missing or zero volatility or volume makes
    /// the cost NaN: the order is rejected.
    class SquareRootImpactCost final : public CostModel
    {
      public:
        SquareRootImpactCost(MatrixView volatility, MatrixView dollar_volume, Real coefficient);
        Real cost(const MarketView &market, const Fill &fill) const override;
        void validate(const MarketData &market) const override;
        std::string describe() const override;

      private:
        MatrixView volatility_;
        MatrixView dollar_volume_;
        Real coefficient_;
    };

    /// Share of the daily traded amount that the executions of a run took
    /// (section 5.2): amount of each execution over the asset's trailing
    /// daily amount on that day.
    struct Participation
    {
        Index executions = 0;
        Real maximum = 0.0;
        Real median = 0.0;
    };

    /// `dollar_volume` and `dates` are those of the market the run was made
    /// on. Executions without a known daily amount are left out of the count.
    Participation participation(const BacktestResult &result, MatrixView dollar_volume, std::span<const Date> dates);

} // namespace quantBacktest

#endif
