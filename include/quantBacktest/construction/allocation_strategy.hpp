#ifndef QUANTBACKTEST_CONSTRUCTION_ALLOCATION_STRATEGY_HPP
#define QUANTBACKTEST_CONSTRUCTION_ALLOCATION_STRATEGY_HPP

#include "quantBacktest/core/run_control.hpp"
#include "quantBacktest/core/types.hpp"
#include "quantBacktest/engine/backtest.hpp"
#include "quantBacktest/engine/strategy.hpp"

#include <optional>
#include <string>
#include <vector>

namespace quantBacktest
{

    enum class AllocationMethod
    {
        EqualWeight,
        MinimumVariance,
        MaximumSharpe,
        RiskParity,
    };

    enum class CovarianceEstimator
    {
        Sample,
        LedoitWolf,
    };

    enum class RebalanceSchedule
    {
        /// Every `rebalance_every` days of the calendar of the data.
        EveryNDays,
        /// On the first day of each month present in the data.
        FirstDayOfMonth,
    };

    struct AllocationConfig
    {
        AllocationMethod method = AllocationMethod::EqualWeight;
        CovarianceEstimator covariance = CovarianceEstimator::LedoitWolf;
        /// Number of daily returns the estimates use, ending on the decision
        /// day. It also defines which assets can be held: those quoted on
        /// every day of the window and members of the universe that day.
        Index lookback = 252;
        /// False: the window slides and keeps `lookback` returns. True: it
        /// grows from the first day of the data, `lookback` being the history
        /// required before the first decision. Both are walk-forward: a
        /// decision only reads what precedes it.
        bool expanding = false;
        RebalanceSchedule schedule = RebalanceSchedule::EveryNDays;
        Index rebalance_every = 21;
        /// Bounds of each weight, held inside the optimisation. Not available
        /// with risk parity.
        Real min_weight = 0.0;
        Real max_weight = 1.0;
        /// Used to annualise the in-sample Sharpe ratio that is recorded.
        Real periods_per_year = 252.0;
    };

    /// What was decided on one rebalancing day.
    struct AllocationRecord
    {
        Date date = 0;
        /// One weight per asset of the market; zero for assets that could not
        /// be held that day.
        std::vector<Real> weights;
        Index eligible = 0;
        /// Annualised Sharpe ratio of these weights **on the window they were
        /// estimated from**. For an optimised portfolio it is a maximum taken
        /// over the data that measure it: its only use is to be compared with
        /// what is realised afterwards (finding 12).
        Real in_sample_sharpe = 0.0;
        /// Shrinkage intensity of the covariance used (0 for the sample one).
        Real shrinkage = 0.0;
    };

    /// A strategy that, on each rebalancing day, re-estimates its inputs on
    /// the trailing window and targets the weights of the chosen method. Every
    /// rebalancing considers every asset again: one left out the first time
    /// can come back (finding 9).
    ///
    /// It reads the market through the view of the decision day only. A
    /// problem without a solution (a cap too low for the number of assets
    /// that can be held, a covariance that is not positive definite, no asset
    /// at all) stops the run with an `Error`.
    class AllocationStrategy final : public Strategy
    {
      public:
        explicit AllocationStrategy(const AllocationConfig &config);

        void decide(const MarketView &market, const BookView &book, Decision &decision) override;

        const std::vector<AllocationRecord> &history() const noexcept { return history_; }

        /// The method and its parameters, in English.
        std::string describe() const;

      private:
        bool rebalances(const MarketView &market) const;

        AllocationConfig config_;
        std::vector<AllocationRecord> history_;
    };

    /// A strategy and the comparison points it is always shown with (finding
    /// 13; companion, section 6.3), on the same assets, the same dates and
    /// the same costs:
    ///
    /// - the equal-weight portfolio, rebalanced on the same days;
    /// - buy and hold of the strategy's first weights.
    ///
    /// - the strategy itself without costs, when it has costs.
    ///
    /// The total-return benchmark is a series the caller already has.
    struct ComparedRuns
    {
        BacktestResult strategy;
        std::vector<AllocationRecord> allocations;
        BacktestResult equal_weight;
        BacktestResult buy_and_hold;
        /// The strategy again with no transaction cost at all: gross and net
        /// side by side (section 5.2). Absent when the run had no cost.
        std::optional<BacktestResult> gross;
        std::string description;
    };

    ComparedRuns run_with_comparisons(const MarketData &market, const AllocationConfig &allocation, const BacktestConfig &backtest,
                                      const RunControl &control = {});

} // namespace quantBacktest

#endif
