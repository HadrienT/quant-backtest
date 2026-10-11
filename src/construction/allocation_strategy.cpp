#include "quantBacktest/construction/allocation_strategy.hpp"

#include "quantBacktest/construction/covariance.hpp"
#include "quantBacktest/construction/optimizer.hpp"
#include "quantBacktest/costs/cost_model.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>

namespace quantBacktest
{

    namespace
    {

        const char *methodName(AllocationMethod method)
        {
            switch (method)
            {
                case AllocationMethod::EqualWeight:
                    return "equal weight";
                case AllocationMethod::MinimumVariance:
                    return "minimum variance";
                case AllocationMethod::MaximumSharpe:
                    return "maximum Sharpe ratio";
                case AllocationMethod::RiskParity:
                    return "risk parity (equal risk contribution)";
            }
            return "";
        }

        unsigned monthOf(Date date)
        {
            const std::chrono::year_month_day ymd{std::chrono::sys_days{std::chrono::days{date}}};
            return static_cast<unsigned>(ymd.month());
        }

        // Forwards the first decision of a strategy, then nothing: buy and
        // hold of its first weights.
        class FirstDecisionOnly final : public Strategy
        {
          public:
            explicit FirstDecisionOnly(Strategy &inner)
                : inner_(inner) {}

            void decide(const MarketView &market, const BookView &book, Decision &decision) override
            {
                if (!done_)
                {
                    inner_.decide(market, book, decision);
                    done_ = decision.kind != DecisionKind::None;
                }
            }

          private:
            Strategy &inner_;
            bool done_ = false;
        };

        RunControl share(const RunControl &control, double from, double width)
        {
            RunControl part;
            part.cancel = control.cancel;
            if (control.progress)
            {
                part.progress = [&control, from, width](double fraction)
                { control.progress(from + width * fraction); };
            }
            return part;
        }

    } // namespace

    AllocationStrategy::AllocationStrategy(const AllocationConfig &config)
        : config_(config)
    {
        if (config.lookback < 2)
        {
            throw Error("the estimation window must hold at least 2 returns");
        }
        if (config.schedule == RebalanceSchedule::EveryNDays && config.rebalance_every == 0)
        {
            throw Error("the rebalancing period must be at least 1 day");
        }
        if (!(config.min_weight >= 0.0) || !(config.max_weight <= 1.0) || !(config.min_weight <= config.max_weight))
        {
            throw Error("the bounds of a weight must satisfy 0 <= minimum <= maximum <= 1: short positions are not supported");
        }
        if (config.method == AllocationMethod::RiskParity && (config.min_weight != 0.0 || config.max_weight != 1.0))
        {
            throw Error("risk parity takes no bounds: a bounded portfolio does not have equal risk contributions");
        }
        if (!(config.periods_per_year > 0.0))
        {
            throw Error("the number of periods per year must be positive");
        }
    }

    std::string AllocationStrategy::describe() const
    {
        char bounds[96];
        std::snprintf(bounds, sizeof(bounds), "each weight between %g %% and %g %%", 100.0 * config_.min_weight, 100.0 * config_.max_weight);
        std::string text = methodName(config_.method);
        if (config_.method != AllocationMethod::EqualWeight)
        {
            text += config_.covariance == CovarianceEstimator::LedoitWolf ? ", Ledoit-Wolf (2004) covariance" : ", sample covariance";
        }
        if (config_.method == AllocationMethod::MaximumSharpe)
        {
            text += ", expected excess returns equal to their mean over the window";
        }
        text += config_.expanding
                    ? ", estimated on every daily return up to the decision day (at least " + std::to_string(config_.lookback) + ")"
                    : ", estimated on the " + std::to_string(config_.lookback) + " daily returns that end on the decision day";
        if (config_.method != AllocationMethod::RiskParity)
        {
            text += std::string(", ") + bounds;
        }
        text += config_.schedule == RebalanceSchedule::FirstDayOfMonth
                    ? ", rebalanced on the first day of each month"
                    : ", rebalanced every " + std::to_string(config_.rebalance_every) + " days";
        text += "; held: the assets quoted on every day of the window and in the universe on the decision day";
        return text;
    }

    bool AllocationStrategy::rebalances(const MarketView &market) const
    {
        const Index t = market.day();
        if (t < config_.lookback)
        {
            return false;
        }
        if (config_.schedule == RebalanceSchedule::EveryNDays)
        {
            return t % config_.rebalance_every == 0;
        }
        return monthOf(market.date(t)) != monthOf(market.date(t - 1));
    }

    void AllocationStrategy::decide(const MarketView &market, const BookView &, Decision &decision)
    {
        if (!rebalances(market))
        {
            return;
        }
        const Index t = market.day();
        const Index assets = market.assets();
        const Index window = config_.expanding ? t : config_.lookback;

        // Assets that can be held: in the universe today, quoted on every day
        // of the window.
        std::vector<Index> eligible;
        for (Index i = 0; i < assets; ++i)
        {
            bool quoted = market.in_universe(t, i);
            for (Index s = t - window; quoted && s <= t; ++s)
            {
                quoted = !is_missing(market.close(s, i));
            }
            if (quoted)
            {
                eligible.push_back(i);
            }
        }
        const Index n = eligible.size();
        if (n == 0)
        {
            throw Error("no asset can be held on " + format_date(market.date(t)) + ": none is quoted over the whole estimation window");
        }

        // Total returns of the window, from raw closes and corporate actions.
        std::vector<Real> returns(window * n);
        std::vector<Real> excess(window * n);
        for (Index k = 0; k < window; ++k)
        {
            const Index s = t - window + 1 + k;
            const Real rate = market.cash_rate(s);
            for (Index j = 0; j < n; ++j)
            {
                const Index i = eligible[j];
                const Real r = market.split_ratio(s, i) * (market.close(s, i) + market.dividend(s, i)) / market.close(s - 1, i) - 1.0;
                returns[k * n + j] = r;
                excess[k * n + j] = r - rate;
            }
        }
        const MatrixView returnsView{returns.data(), window, n};
        const std::vector<Real> lower(n, config_.min_weight);
        const std::vector<Real> upper(n, config_.max_weight);

        std::vector<Real> weights;
        Real shrinkage = 0.0;
        if (config_.method == AllocationMethod::EqualWeight)
        {
            weights = equal_weights(n);
            if (weights.front() > config_.max_weight + 1e-12 || weights.front() < config_.min_weight - 1e-12)
            {
                throw Error("infeasible on " + format_date(market.date(t)) + ": equal weights over " + std::to_string(n) + " assets are outside the bounds");
            }
        }
        else
        {
            const Covariance covariance = config_.covariance == CovarianceEstimator::LedoitWolf ? ledoit_wolf_covariance(returnsView)
                                                                                                : sample_covariance(returnsView);
            shrinkage = covariance.shrinkage;
            try
            {
                switch (config_.method)
                {
                    case AllocationMethod::MinimumVariance:
                        weights = minimum_variance(covariance.view(), lower, upper);
                        break;
                    case AllocationMethod::MaximumSharpe:
                        weights = maximum_sharpe(covariance.view(), mean_returns({excess.data(), window, n}), lower, upper);
                        break;
                    default:
                        weights = risk_parity(covariance.view());
                        break;
                }
            }
            catch (const Error &error)
            {
                throw Error(std::string(error.what()) + " (" + std::to_string(n) + " assets on " + format_date(market.date(t)) + ")");
            }
        }

        // Sharpe ratio of these weights on the window itself.
        Real mean = 0.0;
        std::vector<Real> portfolio(window, 0.0);
        for (Index k = 0; k < window; ++k)
        {
            for (Index j = 0; j < n; ++j)
            {
                portfolio[k] += weights[j] * excess[k * n + j];
            }
            mean += portfolio[k] / static_cast<Real>(window);
        }
        Real squares = 0.0;
        for (const Real x : portfolio)
        {
            squares += (x - mean) * (x - mean);
        }
        const Real deviation = std::sqrt(squares / static_cast<Real>(window - 1));

        AllocationRecord record;
        record.date = market.date(t);
        record.weights.assign(assets, 0.0);
        record.eligible = n;
        record.shrinkage = shrinkage;
        record.in_sample_sharpe = deviation > 0.0 ? mean / deviation * std::sqrt(config_.periods_per_year) : std::numeric_limits<Real>::quiet_NaN();
        for (Index j = 0; j < n; ++j)
        {
            record.weights[eligible[j]] = weights[j];
        }
        decision.kind = DecisionKind::TargetWeights;
        decision.values = record.weights;
        history_.push_back(std::move(record));
    }

    ComparedRuns run_with_comparisons(const MarketData &market, const AllocationConfig &allocation, const BacktestConfig &backtest,
                                      const RunControl &control)
    {
        ComparedRuns out;
        const double runs = backtest.costs != nullptr ? 4.0 : 3.0;
        AllocationStrategy strategy(allocation);
        out.description = strategy.describe();
        out.strategy = run_backtest(market, strategy, backtest, share(control, 0.0, 1.0 / runs));
        out.allocations = strategy.history();

        // The data were controlled by the first run.
        BacktestConfig again = backtest;
        again.data_already_checked = true;

        AllocationConfig equal = allocation;
        equal.method = AllocationMethod::EqualWeight;
        equal.min_weight = 0.0;
        equal.max_weight = 1.0;
        AllocationStrategy equalWeight(equal);
        out.equal_weight = run_backtest(market, equalWeight, again, share(control, 1.0 / runs, 1.0 / runs));

        AllocationStrategy first(allocation);
        FirstDecisionOnly hold(first);
        out.buy_and_hold = run_backtest(market, hold, again, share(control, 2.0 / runs, 1.0 / runs));

        if (backtest.costs != nullptr)
        {
            // A cost of zero, declared: the engine does not execute at the
            // signal's own close without a cost model.
            const ProportionalCost free(0.0);
            again.costs = &free;
            AllocationStrategy gross(allocation);
            out.gross = run_backtest(market, gross, again, share(control, 3.0 / runs, 1.0 / runs));
        }
        control.report(1.0);
        return out;
    }

} // namespace quantBacktest
