#include "quantBacktest/engine/backtest.hpp"

#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <utility>

namespace quantBacktest
{

    namespace
    {

        constexpr Real kNaN = std::numeric_limits<Real>::quiet_NaN();
        // Cash is "not negative" down to this fraction of the portfolio value:
        // the sizing is exact up to rounding.
        constexpr Real kCashTolerance = 1e-10;
        constexpr int kMaxCostIterations = 200;

        // A decision waiting for its execution day.
        struct Pending
        {
            Index decision_day;
            Index due;
            DecisionKind kind;
            std::vector<Real> values;
        };

        // An order sized and costed, before it reaches the book.
        struct Trade
        {
            Index asset;
            Real weight;
            Real requested;
            Real quantity;
            Real cost;
            OrderStatus status;
        };

        class Engine
        {
          public:
            Engine(const MarketData &market, Strategy &strategy, const BacktestConfig &config, const RunControl &control)
                : market_(market),
                  strategy_(strategy),
                  config_(config),
                  control_(control),
                  events_(market),
                  days_(market.dates.size()),
                  assets_(market.assets),
                  atOpen_(config.timing == ExecutionTiming::NextOpen),
                  delay_(config.extra_lag_days + (config.timing == ExecutionTiming::SameClose ? 0 : 1)),
                  cash_(config.initial_capital),
                  shares_(market.assets, 0.0),
                  price_(market.assets, kNaN)
            {
            }

            BacktestResult run()
            {
                const Index start = config_.start_index;
                const Index length = days_ - start;
                reserve(length);
                // Last known closes before the first day, in its share count.
                for (Index t = 0; t < start; ++t)
                {
                    applySplits(t);
                    markToClose(t);
                }
                const Index progressStep = std::max<Index>(1, length / 100);
                for (Index t = start; t < days_; ++t)
                {
                    control_.check_cancelled();
                    day_ = {};
                    if (t > start)
                    {
                        accrueInterest(t);
                    }
                    applySplits(t);
                    payDividends(t);
                    if (atOpen_)
                    {
                        executeDue(t, market_.open);
                    }
                    markToClose(t);
                    if (!atOpen_)
                    {
                        executeDue(t, market_.close);
                    }
                    decide(t);
                    record(t);
                    if ((t - start + 1) % progressStep == 0 || t + 1 == days_)
                    {
                        control_.report(static_cast<double>(t - start + 1) / static_cast<double>(length));
                    }
                }
                return std::move(result_);
            }

          private:
            struct DayTotals
            {
                Real pnl = 0.0;
                Real dividends = 0.0;
                Real interest = 0.0;
                Real costs = 0.0;
                Real traded = 0.0;
                Real turnover = 0.0;
                Index stale = 0;
            };

            void reserve(Index length)
            {
                result_.assets = assets_;
                result_.dates.reserve(length);
                for (std::vector<Real> *series : {&result_.value, &result_.cash, &result_.position_pnl, &result_.dividends,
                                                  &result_.interest, &result_.costs, &result_.traded_notional, &result_.turnover})
                {
                    series->reserve(length);
                }
                result_.stale_positions.reserve(length);
                result_.shares.reserve(length * assets_);
            }

            // Interest over the period that ends on day t, on the balance
            // carried since the previous close.
            void accrueInterest(Index t)
            {
                const std::span<const Real> rates =
                    cash_ < 0.0 && !market_.debit_rate.empty() ? market_.debit_rate : market_.cash_rate;
                if (!rates.empty())
                {
                    day_.interest = cash_ * rates[t];
                    cash_ += day_.interest;
                }
            }

            void applySplits(Index t)
            {
                for (const Split &split : events_.splits_on(t))
                {
                    shares_[split.asset] *= split.ratio;
                    price_[split.asset] /= split.ratio;
                }
            }

            // Credited on the ex-date to whoever held the shares at the
            // previous close: before any execution of the day.
            void payDividends(Index t)
            {
                for (const Dividend &dividend : events_.dividends_on(t))
                {
                    const Real amount = shares_[dividend.asset] * dividend.amount;
                    cash_ += amount;
                    day_.dividends += amount;
                }
            }

            // Moves one asset's reference price, booking the P&L of the
            // position carried across the move.
            void reprice(Index asset, Real price)
            {
                if (shares_[asset] != 0.0)
                {
                    day_.pnl += shares_[asset] * (price - price_[asset]);
                }
                price_[asset] = price;
            }

            // A missing close leaves the last known one: never interpolated.
            void markToClose(Index t)
            {
                const std::span<const Real> close = market_.close.row(t);
                for (Index i = 0; i < assets_; ++i)
                {
                    if (!is_missing(close[i]))
                    {
                        reprice(i, close[i]);
                    }
                    else if (shares_[i] != 0.0)
                    {
                        ++day_.stale;
                    }
                }
            }

            Real valueAt(std::span<const Real> prices) const
            {
                Real value = cash_;
                for (Index i = 0; i < assets_; ++i)
                {
                    if (shares_[i] != 0.0)
                    {
                        value += shares_[i] * (is_missing(prices[i]) ? price_[i] : prices[i]);
                    }
                }
                return value;
            }

            Real costOf(const MarketView &view, Index t, Index asset, Real quantity, Real price) const
            {
                if (config_.costs == nullptr || quantity == 0.0)
                {
                    return 0.0;
                }
                const Real cost = config_.costs->cost(view, Fill{t, asset, quantity, price});
                if (cost < 0.0 || std::isinf(cost))
                {
                    throw Error("the cost model returned a negative or infinite cost");
                }
                return cost;
            }

            void decide(Index t)
            {
                const MarketView view(market_, events_, t);
                const BookView book{cash_, valueAt(price_), shares_, price_};
                decision_.kind = DecisionKind::None;
                decision_.values.assign(assets_, kNaN);
                strategy_.decide(view, book, decision_);
                if (decision_.kind == DecisionKind::None)
                {
                    return;
                }
                validate(view, t);
                Pending pending{t, t + delay_, decision_.kind, decision_.values};
                if (delay_ == 0)
                {
                    execute(pending, t, market_.close);
                }
                else if (pending.due < days_)
                {
                    pending_.push_back(std::move(pending));
                }
            }

            void validate(const MarketView &view, Index t) const
            {
                if (decision_.values.size() != assets_)
                {
                    throw Error("the strategy must return one value per asset");
                }
                const bool weights = decision_.kind == DecisionKind::TargetWeights;
                for (Index i = 0; i < assets_; ++i)
                {
                    const Real x = decision_.values[i];
                    if (is_missing(x))
                    {
                        continue;
                    }
                    const std::string where = " for asset " + std::to_string(i) + " on " + format_date(market_.dates[t]);
                    if (std::isinf(x))
                    {
                        throw Error("the strategy returned an infinite value" + where);
                    }
                    if (weights && x < 0.0)
                    {
                        throw Error("short positions are not supported: negative target weight" + where);
                    }
                    if (x > 0.0 && !view.in_universe(t, i))
                    {
                        throw Error("the strategy buys an asset that is outside the universe" + where);
                    }
                }
            }

            void executeDue(Index t, const MatrixView &venue)
            {
                while (!pending_.empty() && pending_.front().due == t)
                {
                    execute(pending_.front(), t, venue);
                    pending_.pop_front();
                }
            }

            void journal(const Pending &order, Index t, const Trade &trade, Real price)
            {
                result_.journal.push_back({market_.dates[order.decision_day], market_.dates[t], trade.asset,
                                           trade.requested, trade.quantity, price, trade.cost, trade.status});
            }

            void execute(const Pending &order, Index t, const MatrixView &venue)
            {
                const std::span<const Real> prices = venue.row(t);
                const MarketView view(market_, events_, t);
                const Real value = valueAt(prices);
                trades_.clear();

                // Orders on assets without a price today are rejected: the day
                // is not tradable for them.
                for (Index i = 0; i < assets_; ++i)
                {
                    const Real x = order.values[i];
                    if (is_missing(x) || (order.kind == DecisionKind::Orders && x == 0.0))
                    {
                        continue;
                    }
                    if (!is_missing(prices[i]))
                    {
                        trades_.push_back({i, x, x, x, 0.0, OrderStatus::Filled});
                        continue;
                    }
                    Real requested = x;
                    if (order.kind == DecisionKind::TargetWeights)
                    {
                        requested = x == 0.0 ? -shares_[i] : x * value / price_[i] - shares_[i];
                    }
                    if (requested != 0.0)
                    {
                        journal(order, t, {i, x, requested, 0.0, 0.0, OrderStatus::RejectedNoQuote}, kNaN);
                    }
                }

                if (order.kind == DecisionKind::TargetWeights)
                {
                    sizeTargetWeights(order, view, t, prices, value);
                }
                else
                {
                    costOrders(order, view, t, prices);
                }
                if (!config_.allow_borrowing)
                {
                    fitToCash(view, t, prices, value);
                }

                for (const Trade &trade : trades_)
                {
                    const Index i = trade.asset;
                    const Real price = prices[i];
                    if (trade.quantity != 0.0)
                    {
                        if (shares_[i] + trade.quantity < -1e-12 * std::max(1.0, std::abs(shares_[i])))
                        {
                            throw Error("short positions are not supported: the order sells more than is held, for asset " +
                                        std::to_string(i) + " on " + format_date(market_.dates[t]));
                        }
                        reprice(i, price);
                        shares_[i] = trade.weight == 0.0 && order.kind == DecisionKind::TargetWeights ? 0.0 : std::max(shares_[i] + trade.quantity, 0.0);
                        cash_ -= trade.quantity * price + trade.cost;
                        day_.costs += trade.cost;
                        day_.traded += std::abs(trade.quantity) * price;
                    }
                    if (trade.requested != 0.0)
                    {
                        journal(order, t, trade, price);
                    }
                }
                if (value != 0.0)
                {
                    day_.turnover = 0.5 * day_.traded / std::abs(value);
                }
            }

            // Target weights are fractions of the value left after the costs
            // of reaching them: the cost depends on the trades and the trades
            // on the cost, a fixed point that a few iterations reach.
            void sizeTargetWeights(const Pending &order, const MarketView &view, Index t, std::span<const Real> prices, Real value)
            {
                Real totalCost = 0.0;
                for (int iteration = 0;; ++iteration)
                {
                    Real newCost = 0.0;
                    bool dropped = false;
                    for (Trade &trade : trades_)
                    {
                        const Index i = trade.asset;
                        trade.quantity = trade.weight == 0.0 ? -shares_[i] : trade.weight * (value - totalCost) / prices[i] - shares_[i];
                        trade.requested = trade.quantity;
                        trade.cost = costOf(view, t, i, trade.quantity, prices[i]);
                        if (is_missing(trade.cost))
                        {
                            journal(order, t, {i, trade.weight, trade.requested, 0.0, 0.0, OrderStatus::RejectedNoCostEstimate}, prices[i]);
                            trade.status = OrderStatus::RejectedNoCostEstimate;
                            dropped = true;
                        }
                        else
                        {
                            newCost += trade.cost;
                        }
                    }
                    if (dropped)
                    {
                        std::erase_if(trades_, [](const Trade &trade)
                                      { return trade.status == OrderStatus::RejectedNoCostEstimate; });
                        totalCost = 0.0;
                        iteration = 0;
                        continue;
                    }
                    const bool converged = std::abs(newCost - totalCost) <= 1e-15 * std::abs(value);
                    totalCost = newCost;
                    if (converged)
                    {
                        return;
                    }
                    if (iteration >= kMaxCostIterations)
                    {
                        throw Error("the transaction costs are too large for the target weights to be reached on " + format_date(market_.dates[t]));
                    }
                }
            }

            void costOrders(const Pending &order, const MarketView &view, Index t, std::span<const Real> prices)
            {
                for (Trade &trade : trades_)
                {
                    trade.cost = costOf(view, t, trade.asset, trade.quantity, prices[trade.asset]);
                    if (is_missing(trade.cost))
                    {
                        journal(order, t, {trade.asset, trade.weight, trade.requested, 0.0, 0.0, OrderStatus::RejectedNoCostEstimate}, prices[trade.asset]);
                        trade.status = OrderStatus::RejectedNoCostEstimate;
                    }
                }
                std::erase_if(trades_, [](const Trade &trade)
                              { return trade.status == OrderStatus::RejectedNoCostEstimate; });
            }

            // Without borrowing, the purchases are scaled down together until
            // cash stays at zero. It only happens when a sale was rejected or
            // when the strategy asks for more than the book holds.
            void fitToCash(const MarketView &view, Index t, std::span<const Real> prices, Real value)
            {
                const auto cashAfter = [&](Real scale)
                {
                    Real cash = cash_;
                    for (const Trade &trade : trades_)
                    {
                        const Real price = prices[trade.asset];
                        if (trade.quantity > 0.0)
                        {
                            const Real quantity = scale * trade.quantity;
                            cash -= quantity * price + costOf(view, t, trade.asset, quantity, price);
                        }
                        else
                        {
                            cash -= trade.quantity * price + trade.cost;
                        }
                    }
                    return cash;
                };
                if (cashAfter(1.0) >= -kCashTolerance * std::abs(value))
                {
                    return;
                }
                Real low = 0.0;
                Real high = 1.0;
                for (int iteration = 0; iteration < 60; ++iteration)
                {
                    const Real middle = 0.5 * (low + high);
                    (cashAfter(middle) >= 0.0 ? low : high) = middle;
                }
                for (Trade &trade : trades_)
                {
                    if (trade.quantity > 0.0)
                    {
                        trade.quantity *= low;
                        trade.cost = costOf(view, t, trade.asset, trade.quantity, prices[trade.asset]);
                        trade.status = OrderStatus::PartiallyFilled;
                    }
                }
            }

            void record(Index t)
            {
                result_.dates.push_back(market_.dates[t]);
                result_.value.push_back(valueAt(price_));
                result_.cash.push_back(cash_);
                result_.position_pnl.push_back(day_.pnl);
                result_.dividends.push_back(day_.dividends);
                result_.interest.push_back(day_.interest);
                result_.costs.push_back(day_.costs);
                result_.traded_notional.push_back(day_.traded);
                result_.turnover.push_back(day_.turnover);
                result_.stale_positions.push_back(day_.stale);
                result_.shares.insert(result_.shares.end(), shares_.begin(), shares_.end());
            }

            const MarketData &market_;
            Strategy &strategy_;
            const BacktestConfig &config_;
            const RunControl &control_;
            EventIndex events_;
            Index days_;
            Index assets_;
            bool atOpen_;
            Index delay_;

            Real cash_;
            std::vector<Real> shares_;
            // Last known price per asset, in the current share count.
            std::vector<Real> price_;
            DayTotals day_;
            Decision decision_;
            std::deque<Pending> pending_;
            std::vector<Trade> trades_;
            BacktestResult result_;
        };

        Conventions describe(const MarketData &market, const BacktestConfig &config)
        {
            Conventions c;
            const std::string lag = config.extra_lag_days == 0 ? "" : ", plus " + std::to_string(config.extra_lag_days) + " day(s) of extra delay";
            switch (config.timing)
            {
                case ExecutionTiming::NextOpen:
                    c.execution = "decision after the close of day t with data up to t, execution at the open of t+1" + lag;
                    break;
                case ExecutionTiming::NextClose:
                    c.execution = "decision after the close of day t with data up to t, execution at the close of t+1" + lag;
                    break;
                case ExecutionTiming::SameClose:
                    c.execution = "decision at the close of day t with data up to t, execution at that same close by an on-close order" + lag;
                    break;
            }
            c.execution += "; an order without a price on its execution day is rejected, not carried over";
            c.valuation = "value = cash + shares x last known raw close; a missing close is never interpolated, the position keeps its last price";
            c.costs = config.costs == nullptr ? "no transaction cost" : config.costs->describe() + "; paid in cash, target weights are fractions of the value net of costs";
            c.cash = market.cash_rate.empty()
                         ? "cash earns no interest (no rate series was given)"
                         : "cash earns the simple rate given for each period, applied to the balance of the previous close";
            c.cash += config.allow_borrowing ? "; a negative balance pays the debit rate" : "; no borrowing: purchases are scaled down to the cash available";
            c.corporate_actions = "splits multiply the shares and dividends are credited in cash, both on their effective date and before any execution of that day";
            c.turnover = "half the amount traded over the portfolio value at execution; cash is not counted as an asset";
            return c;
        }

    } // namespace

    BacktestResult run_backtest(const MarketData &market, Strategy &strategy, const BacktestConfig &config, const RunControl &control)
    {
        QualityReport quality = config.data_already_checked ? check_market_structure(market) : check_market_data(market, config.quality);
        if (!quality.usable())
        {
            throw Error("the data cannot be used:\n" + quality.error_summary());
        }
        if (!(std::isfinite(config.initial_capital) && config.initial_capital > 0.0))
        {
            throw Error("the initial capital must be a positive number");
        }
        if (config.start_index >= market.dates.size())
        {
            throw Error("the first day of the book is after the last date");
        }
        if (config.timing == ExecutionTiming::NextOpen && market.open.empty())
        {
            throw Error("executing at the open needs the 'open' table");
        }
        if (config.timing == ExecutionTiming::SameClose && config.costs == nullptr)
        {
            throw Error("executing at the close the signal was computed on needs an explicit cost model");
        }
        if (config.costs != nullptr)
        {
            config.costs->validate(market);
        }

        Engine engine(market, strategy, config, control);
        BacktestResult result = engine.run();
        result.quality = std::move(quality);
        result.conventions = describe(market, config);
        return result;
    }

} // namespace quantBacktest
