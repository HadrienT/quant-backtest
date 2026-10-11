#ifndef QUANTBACKTEST_ENGINE_BACKTEST_HPP
#define QUANTBACKTEST_ENGINE_BACKTEST_HPP

#include "quantBacktest/core/run_control.hpp"
#include "quantBacktest/core/types.hpp"
#include "quantBacktest/costs/cost_model.hpp"
#include "quantBacktest/data/market_data.hpp"
#include "quantBacktest/engine/strategy.hpp"
#include "quantBacktest/quality/quality_report.hpp"

#include <string>
#include <vector>

namespace quantBacktest
{

    /// When a decision taken after the close of day t is executed (companion,
    /// section 4.2). The convention is declared in the result.
    enum class ExecutionTiming
    {
        /// At the open of t+1. Needs the 'open' table.
        NextOpen,
        /// At the close of t+1: a full day late, the prudent choice.
        NextClose,
        /// At the close of t itself, by an on-close order. The signal has
        /// then seen the price it trades at: accepted only with a cost model.
        SameClose,
    };

    struct BacktestConfig
    {
        /// Cash on the first day.
        Real initial_capital = 0.0;
        ExecutionTiming timing = ExecutionTiming::NextClose;
        /// Days added to the timing above. Rerunning with one more day is the
        /// classic check that a strategy does not depend on its execution
        /// instant.
        Index extra_lag_days = 0;
        /// Index of the first day of the book. Earlier rows are history, for
        /// the strategy and for the cost estimates.
        Index start_index = 0;
        /// If false, purchases are scaled down so that cash never goes
        /// negative. If true, a negative balance pays the debit rate.
        bool allow_borrowing = false;
        /// If true, a position can be negative: the proceeds of the sale go to
        /// cash, the dividends detached are paid, and the value of the shares
        /// borrowed pays `borrow_rate`, which the data must then hold.
        bool allow_short = false;
        /// If true, every order is rounded towards zero to a whole number of
        /// shares; what it does not spend stays in cash. A split can still
        /// leave a fraction of a share.
        bool whole_shares = false;
        /// Not owned; null means no transaction cost.
        const CostModel *costs = nullptr;
        QualityThresholds quality;
        /// Set it when the same data went through `check_market_data` once
        /// and the run is repeated (a parameter grid, a validation scheme):
        /// only shapes, dates, rates and events are verified again, and the
        /// report of the result holds those controls only.
        bool data_already_checked = false;
    };

    enum class OrderStatus
    {
        Filled,
        /// Purchase scaled down for lack of cash.
        PartiallyFilled,
        /// No price on the execution day: the day is not tradable for this
        /// asset. The order is dropped, not carried over.
        RejectedNoQuote,
        /// The cost model could not estimate the cost of the fill.
        RejectedNoCostEstimate,
    };

    /// One line of the order journal (companion, section 4.3).
    struct OrderRecord
    {
        Date decision_date = 0;
        Date execution_date = 0;
        Index asset = 0;
        /// Shares asked for, positive to buy; NaN if it could not be sized.
        Real requested = 0.0;
        /// Shares executed; zero if rejected.
        Real quantity = 0.0;
        /// Execution price; NaN if rejected for lack of a quote.
        Real price = 0.0;
        /// Transaction cost paid in cash.
        Real cost = 0.0;
        OrderStatus status = OrderStatus::Filled;
    };

    /// What each figure means. In English, ready to be shown.
    struct Conventions
    {
        std::string execution;
        std::string valuation;
        std::string costs;
        std::string cash;
        std::string corporate_actions;
        std::string turnover;
    };

    /// Every series has one value per day, from `start_index` to the end.
    /// Each day:
    ///
    ///     value = cash + sum of shares x last known close
    ///     value - previous value = position_pnl + dividends + interest - costs - borrow_cost
    ///
    /// On the first day the previous value is the initial capital.
    struct BacktestResult
    {
        std::vector<Date> dates;
        std::vector<Real> value;
        std::vector<Real> cash;
        std::vector<Real> position_pnl;
        std::vector<Real> dividends;
        std::vector<Real> interest;
        std::vector<Real> costs;
        /// Fee paid on the shares borrowed for the short positions.
        std::vector<Real> borrow_cost;
        /// Sum of |quantity| x price over the day's executions.
        std::vector<Real> traded_notional;
        /// Half the traded notional over the portfolio value at execution.
        std::vector<Real> turnover;
        /// Number of positions valued at a close older than the day's.
        std::vector<Index> stale_positions;
        /// Shares held at the end of each day: one row per day, row-major.
        std::vector<Real> shares;
        Index assets = 0;

        std::vector<OrderRecord> journal;
        QualityReport quality;
        Conventions conventions;
    };

    /// Runs a strategy over the data: a book in shares and cash, one day at a
    /// time (companion, section 4).
    ///
    /// Throws `Error` if the data fail the quality controls, if the
    /// configuration is inconsistent, or if the strategy asks for something
    /// the engine was not allowed to do (a short position, an asset outside
    /// the universe); `Cancelled` if `control.cancel` is raised.
    BacktestResult run_backtest(const MarketData &market, Strategy &strategy, const BacktestConfig &config, const RunControl &control = {});

} // namespace quantBacktest

#endif
