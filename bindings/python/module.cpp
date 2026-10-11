#include <pybind11/functional.h>
#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "quantBacktest/attribution/factor_regression.hpp"
#include "quantBacktest/construction/allocation_strategy.hpp"
#include "quantBacktest/construction/grid.hpp"
#include "quantBacktest/core/build_info.hpp"
#include "quantBacktest/core/error.hpp"
#include "quantBacktest/core/fingerprint.hpp"
#include "quantBacktest/core/run_control.hpp"
#include "quantBacktest/costs/cost_model.hpp"
#include "quantBacktest/costs/impact.hpp"
#include "quantBacktest/costs/spread_estimators.hpp"
#include "quantBacktest/data/returns.hpp"
#include "quantBacktest/inference/performance_report.hpp"
#include "quantBacktest/inference/sharpe_difference.hpp"
#include "quantBacktest/metrics/cost_report.hpp"
#include "quantBacktest/risk_models/var_backtest.hpp"
#include "quantBacktest/validation/cpcv.hpp"
#include "quantBacktest/validation/overfitting.hpp"
#include "quantBacktest/validation/walk_forward.hpp"

#ifdef QB_HAS_HEDGING
#include "quantBacktest/hedging/hedging_backtest.hpp"
#endif

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

namespace py = pybind11;
namespace qb = quantBacktest;

// The interface carries plain data only (docs/contract.md, section 3): numpy
// arrays, numbers, strings, dictionaries. No C++ object is exposed.

namespace
{

    using Array = py::array_t<double, py::array::c_style | py::array::forcecast>;
    using Callback = std::optional<py::function>;

    std::span<const double> vector(const Array &array, const char *name)
    {
        if (array.ndim() != 1)
        {
            throw qb::Error(std::string("'") + name + "' must be a one-dimensional array");
        }
        return {array.data(), static_cast<std::size_t>(array.size())};
    }

    py::dict toDict(const qb::Estimate &e)
    {
        py::dict out;
        out["value"] = e.value;
        out["standard_error"] = e.standard_error;
        out["lower"] = e.lower;
        out["upper"] = e.upper;
        out["confidence"] = e.confidence;
        out["method"] = e.method;
        out["convention"] = e.convention;
        return out;
    }

    // Long calls release the GIL. Progress is reported from the calling
    // thread: the callback takes the GIL back, calls `progress`, then asks
    // `cancel` whether to stop.
    class Control
    {
      public:
        Control(const Callback &progress, const Callback &cancel)
        {
            control_.cancel = &cancelled_;
            if (progress || cancel)
            {
                control_.progress = [this, progress, cancel](double fraction)
                {
                    const py::gil_scoped_acquire gil;
                    if (progress)
                    {
                        (*progress)(fraction);
                    }
                    if (cancel && py::cast<bool>((*cancel)()))
                    {
                        cancelled_.store(true);
                    }
                };
            }
        }

        const qb::RunControl &get() const { return control_; }

      private:
        std::atomic<bool> cancelled_{false};
        qb::RunControl control_;
    };

    // The parameters are in the order of the py::arg list at the bottom.
    py::dict performanceReport(const Array &returns, const Array &cashRate, const std::optional<Array> &benchmark,
                               std::size_t bootstrapReplications, double meanBlockLength, std::uint64_t seed,
                               double periodsPerYear, double confidence, double tail, std::size_t threads,
                               const Callback &progress, const Callback &cancel)
    {
        qb::PerformanceConfig config;
        config.periods_per_year = periodsPerYear;
        config.confidence = confidence;
        config.tail = tail;
        config.bootstrap.replications = bootstrapReplications;
        config.bootstrap.mean_block_length = meanBlockLength;
        config.bootstrap.seed = seed;
        config.bootstrap.threads = threads;

        const std::span<const double> r = vector(returns, "returns");
        const std::span<const double> c = vector(cashRate, "cash_rate");
        const std::span<const double> b = benchmark ? vector(*benchmark, "benchmark") : std::span<const double>{};

        const Control control(progress, cancel);
        qb::PerformanceReport report;
        {
            const py::gil_scoped_release release;
            report = qb::performance_report(r, c, b, config, control.get());
        }

        py::dict metrics;
        for (const qb::NamedEstimate &metric : report.metrics)
        {
            metrics[py::str(metric.name)] = toDict(metric.estimate);
        }
        py::dict omitted;
        for (const qb::OmittedMetric &metric : report.omitted)
        {
            omitted[py::str(metric.name)] = metric.reason;
        }
        py::dict out;
        out["observations"] = report.observations;
        out["periods_per_year"] = report.periods_per_year;
        out["years"] = report.years;
        out["metrics"] = metrics;
        out["omitted"] = omitted;
        out["t_statistic"] = report.t_statistic;
        out["probabilistic_sharpe_ratio"] = report.probabilistic_sharpe_ratio;
        out["minimum_track_record_years"] = report.minimum_track_record_years;
        return out;
    }

    py::dict factorRegression(const Array &excess, const Array &factors, std::size_t neweyWestLags, double periodsPerYear, double confidence)
    {
        if (factors.ndim() != 2)
        {
            throw qb::Error("'factors' must be a two-dimensional array, one row per period and one column per factor");
        }
        qb::FactorRegressionConfig config;
        config.periods_per_year = periodsPerYear;
        config.confidence = confidence;
        config.newey_west_lags = neweyWestLags;
        const qb::MatrixView view(factors.data(), static_cast<std::size_t>(factors.shape(0)), static_cast<std::size_t>(factors.shape(1)));
        const qb::FactorRegression r = qb::factor_regression(vector(excess, "excess"), view, config);

        py::list betas;
        for (const qb::Estimate &beta : r.betas)
        {
            betas.append(toDict(beta));
        }
        py::dict out;
        out["alpha"] = toDict(r.alpha);
        out["betas"] = betas;
        out["alpha_t_statistic"] = r.alpha_t_statistic;
        out["beta_t_statistics"] = r.beta_t_statistics;
        out["r_squared"] = r.r_squared;
        out["residual_volatility"] = r.residual_volatility;
        out["observations"] = r.observations;
        out["newey_west_lags"] = r.newey_west_lags;
        return out;
    }

    py::dict toDict(const qb::LikelihoodRatio &test)
    {
        py::dict out;
        out["statistic"] = test.statistic;
        out["p_value"] = test.p_value;
        out["degrees_of_freedom"] = test.degrees_of_freedom;
        return out;
    }

    py::dict varBacktest(const Array &pnl, const Array &valueAtRisk, double tailProbability)
    {
        const std::vector<std::uint8_t> exceptions = qb::var_exceptions(vector(pnl, "pnl"), vector(valueAtRisk, "value_at_risk"));
        const qb::CoverageTests tests = qb::var_coverage_tests(exceptions, tailProbability);
        const qb::TrafficLight light = qb::traffic_light(tests.exceptions, tests.observations, tailProbability);

        py::dict zone;
        zone["zone"] = qb::zone_name(light.zone);
        zone["cumulative_probability"] = light.cumulative_probability;
        zone["basel_plus_factor"] = light.basel_plus_factor;
        zone["frtb_multiplier"] = light.frtb_multiplier;

        py::dict out;
        out["observations"] = tests.observations;
        out["exceptions"] = tests.exceptions;
        out["expected_exceptions"] = tests.expected_exceptions;
        out["exception_days"] = py::array_t<std::uint8_t>(static_cast<py::ssize_t>(exceptions.size()), exceptions.data());
        out["unconditional_coverage"] = toDict(tests.unconditional_coverage);
        out["independence"] = toDict(tests.independence);
        out["conditional_coverage"] = toDict(tests.conditional_coverage);
        out["transitions"] = py::make_tuple(py::make_tuple(tests.transitions[0][0], tests.transitions[0][1]),
                                            py::make_tuple(tests.transitions[1][0], tests.transitions[1][1]));
        out["traffic_light"] = zone;
        return out;
    }

    py::dict sharpeDifference(const Array &first, const Array &second, std::size_t bootstrapReplications, double meanBlockLength,
                              std::uint64_t seed, double periodsPerYear, double confidence, std::size_t threads,
                              const Callback &progress, const Callback &cancel)
    {
        qb::BootstrapConfig bootstrap;
        bootstrap.replications = bootstrapReplications;
        bootstrap.mean_block_length = meanBlockLength;
        bootstrap.seed = seed;
        bootstrap.threads = threads;
        const std::span<const double> a = vector(first, "first");
        const std::span<const double> b = vector(second, "second");
        const Control control(progress, cancel);
        qb::SharpeDifference d;
        {
            const py::gil_scoped_release release;
            d = qb::sharpe_difference(a, b, periodsPerYear, confidence, bootstrap, control.get());
        }
        py::dict out;
        out["analytical"] = toDict(d.analytical);
        out["z_statistic"] = d.z_statistic;
        out["p_value"] = d.p_value;
        out["correlation"] = d.correlation;
        out["bootstrap"] = d.bootstrap ? py::object(toDict(*d.bootstrap)) : py::object(py::none());
        out["bootstrap_p_value"] = d.bootstrap_p_value;
        return out;
    }

    // ---- The strategy backtest ----

    // datetime64 of any unit, to days since 1970-01-01.
    std::vector<qb::Date> toDates(const py::object &dates, const char *name)
    {
        const py::array array = py::array::ensure(dates);
        if (!array || array.dtype().kind() != 'M' || array.ndim() != 1)
        {
            throw qb::Error(std::string("'") + name + "' must be a one-dimensional datetime64 array");
        }
        const auto days = py::array_t<std::int64_t>(array.attr("astype")("datetime64[D]").attr("astype")("int64"));
        std::vector<qb::Date> out(static_cast<std::size_t>(days.size()));
        for (py::ssize_t k = 0; k < days.size(); ++k)
        {
            out[static_cast<std::size_t>(k)] = static_cast<qb::Date>(days.at(k));
        }
        return out;
    }

    py::object fromDates(const std::vector<qb::Date> &dates)
    {
        py::array_t<std::int64_t> days(static_cast<py::ssize_t>(dates.size()));
        for (std::size_t k = 0; k < dates.size(); ++k)
        {
            days.mutable_at(static_cast<py::ssize_t>(k)) = dates[k];
        }
        return days.attr("astype")("datetime64[D]");
    }

    qb::MatrixView table(const std::optional<Array> &array, const char *name)
    {
        if (!array)
        {
            return {};
        }
        if (array->ndim() != 2)
        {
            throw qb::Error(std::string("'") + name + "' must be a two-dimensional array, one row per date and one column per asset");
        }
        return {array->data(), static_cast<std::size_t>(array->shape(0)), static_cast<std::size_t>(array->shape(1))};
    }

    py::array_t<double> toArray(const std::vector<double> &values)
    {
        return py::array_t<double>(static_cast<py::ssize_t>(values.size()), values.data());
    }

    py::array_t<double> toArray(const std::vector<double> &values, std::size_t rows, std::size_t cols)
    {
        py::array_t<double> out({static_cast<py::ssize_t>(rows), static_cast<py::ssize_t>(cols)});
        std::copy(values.begin(), values.end(), out.mutable_data());
        return out;
    }

    const char *statusName(qb::OrderStatus status)
    {
        switch (status)
        {
            case qb::OrderStatus::Filled:
                return "filled";
            case qb::OrderStatus::PartiallyFilled:
                return "partially_filled";
            case qb::OrderStatus::RejectedNoQuote:
                return "rejected_no_quote";
            case qb::OrderStatus::RejectedNoCostEstimate:
                return "rejected_no_cost_estimate";
        }
        return "";
    }

    py::dict toDict(const qb::BacktestResult &r)
    {
        const std::size_t days = r.dates.size();
        py::dict out;
        out["dates"] = fromDates(r.dates);
        out["value"] = toArray(r.value);
        out["cash"] = toArray(r.cash);
        out["position_pnl"] = toArray(r.position_pnl);
        out["dividends"] = toArray(r.dividends);
        out["interest"] = toArray(r.interest);
        out["costs"] = toArray(r.costs);
        out["borrow_cost"] = toArray(r.borrow_cost);
        out["traded_notional"] = toArray(r.traded_notional);
        out["turnover"] = toArray(r.turnover);
        out["stale_positions"] = py::array_t<std::uint64_t>(static_cast<py::ssize_t>(days), r.stale_positions.data());
        out["shares"] = toArray(r.shares, days, r.assets);

        const std::size_t orders = r.journal.size();
        std::vector<qb::Date> decided(orders);
        std::vector<qb::Date> executed(orders);
        py::array_t<std::uint64_t> asset(static_cast<py::ssize_t>(orders));
        std::vector<double> requested(orders);
        std::vector<double> quantity(orders);
        std::vector<double> price(orders);
        std::vector<double> cost(orders);
        py::list status;
        for (std::size_t k = 0; k < orders; ++k)
        {
            const qb::OrderRecord &order = r.journal[k];
            decided[k] = order.decision_date;
            executed[k] = order.execution_date;
            asset.mutable_at(static_cast<py::ssize_t>(k)) = order.asset;
            requested[k] = order.requested;
            quantity[k] = order.quantity;
            price[k] = order.price;
            cost[k] = order.cost;
            status.append(statusName(order.status));
        }
        py::dict journal;
        journal["decision_date"] = fromDates(decided);
        journal["execution_date"] = fromDates(executed);
        journal["asset"] = asset;
        journal["requested"] = toArray(requested);
        journal["quantity"] = toArray(quantity);
        journal["price"] = toArray(price);
        journal["cost"] = toArray(cost);
        journal["status"] = status;
        out["journal"] = journal;
        return out;
    }

    template <typename Enum>
    Enum choose(const std::string &value, const char *name, std::initializer_list<std::pair<const char *, Enum>> choices)
    {
        std::string known;
        for (const auto &[text, result] : choices)
        {
            if (value == text)
            {
                return result;
            }
            known += known.empty() ? "" : ", ";
            known += text;
        }
        throw qb::Error(std::string("unknown ") + name + " '" + value + "': expected one of " + known);
    }

    // Events: (assets, dates, amounts or ratios), three arrays of one length.
    void readEvents(const py::object &given, const char *name, std::vector<std::size_t> &assets, std::vector<qb::Date> &when, std::vector<double> &values)
    {
        assets.clear();
        when.clear();
        values.clear();
        if (given.is_none())
        {
            return;
        }
        const py::tuple parts = py::tuple(given);
        if (parts.size() != 3)
        {
            throw qb::Error(std::string("'") + name + "' must be a tuple of three arrays: assets, dates, values");
        }
        assets = py::cast<std::vector<std::size_t>>(parts[0]);
        when = toDates(parts[1], name);
        values = py::cast<std::vector<double>>(parts[2]);
        if (assets.size() != when.size() || assets.size() != values.size())
        {
            throw qb::Error(std::string("the three arrays of '") + name + "' must have the same length");
        }
    }

    std::vector<qb::Dividend> toDividends(const py::object &given)
    {
        std::vector<std::size_t> assets;
        std::vector<qb::Date> when;
        std::vector<double> values;
        readEvents(given, "dividends", assets, when, values);
        std::vector<qb::Dividend> out;
        for (std::size_t k = 0; k < assets.size(); ++k)
        {
            out.push_back({assets[k], when[k], values[k]});
        }
        return out;
    }

    std::vector<qb::Split> toSplits(const py::object &given)
    {
        std::vector<std::size_t> assets;
        std::vector<qb::Date> when;
        std::vector<double> values;
        readEvents(given, "splits", assets, when, values);
        std::vector<qb::Split> out;
        for (std::size_t k = 0; k < assets.size(); ++k)
        {
            out.push_back({assets[k], when[k], values[k]});
        }
        return out;
    }

    py::array_t<double> totalReturns(const py::object &dates, const Array &close, const py::object &dividends, const py::object &splits)
    {
        const std::vector<qb::Date> days = toDates(dates, "dates");
        if (close.ndim() != 2 || static_cast<std::size_t>(close.shape(0)) != days.size())
        {
            throw qb::Error("'close' must be a two-dimensional array, one row per date and one column per asset");
        }
        const std::vector<qb::Dividend> dividendList = toDividends(dividends);
        const std::vector<qb::Split> splitList = toSplits(splits);
        qb::MarketData market;
        market.dates = days;
        market.assets = static_cast<std::size_t>(close.shape(1));
        market.close = {close.data(), days.size(), market.assets};
        market.dividends = dividendList;
        market.splits = splitList;
        return toArray(qb::total_returns(market), days.size(), market.assets);
    }

    py::array_t<double> periodRates(const py::object &dates, const Array &annualRate, double basis)
    {
        const std::vector<qb::Date> days = toDates(dates, "dates");
        return toArray(qb::period_rates_from_annualized(days, vector(annualRate, "annual_rate"), basis));
    }

    using Members = std::optional<py::array_t<bool, py::array::c_style | py::array::forcecast>>;

    // Everything a strategy backtest shares with a grid of them: the market,
    // the costs and the execution rules. It holds pointers into itself and
    // into the arrays it was given: built in place, never copied.
    struct Setup
    {
        std::vector<qb::Date> days;
        std::vector<std::uint8_t> members;
        std::vector<qb::Dividend> dividends;
        std::vector<qb::Split> splits;
        qb::MarketData market;

        std::optional<qb::ProportionalCost> proportional;
        std::vector<double> spreads;
        std::vector<double> volatility;
        std::vector<double> dollarVolume;
        std::unique_ptr<qb::SpreadCost> spread;
        std::unique_ptr<qb::SquareRootImpactCost> impact;
        std::unique_ptr<qb::CostStack> stack;
        qb::BacktestConfig backtest;

        Setup(const Setup &) = delete;
        Setup &operator=(const Setup &) = delete;

        Setup(const py::object &dates, const Array &close,
              const std::optional<Array> &open, const std::optional<Array> &high, const std::optional<Array> &low, const std::optional<Array> &volume,
              const Members &universe, const std::optional<Array> &cashRate, const std::optional<Array> &borrowRate,
              const py::object &dividendEvents, const py::object &splitEvents,
              double initialCapital, double proportionalCost,
              const std::string &timing, std::size_t extraLagDays, std::size_t startIndex, bool allowBorrowing, bool allowShort, bool wholeShares,
              const py::object &spreadEstimator, std::size_t spreadWindow, std::size_t spreadMinPairs,
              const py::object &impactCoefficient, std::size_t impactWindow)
            : days(toDates(dates, "dates")), dividends(toDividends(dividendEvents)), splits(toSplits(splitEvents))
        {
            if (close.ndim() != 2 || static_cast<std::size_t>(close.shape(0)) != days.size())
            {
                throw qb::Error("'close' must be a two-dimensional array, one row per date and one column per asset");
            }
            market.dates = days;
            market.assets = static_cast<std::size_t>(close.shape(1));
            market.close = {close.data(), days.size(), market.assets};
            market.open = table(open, "open");
            market.high = table(high, "high");
            market.low = table(low, "low");
            market.volume = table(volume, "volume");
            if (universe)
            {
                if (universe->ndim() != 2)
                {
                    throw qb::Error("'universe' must be a two-dimensional array, one row per date and one column per asset");
                }
                members.assign(universe->data(), universe->data() + universe->size());
                market.universe = members;
            }
            if (cashRate)
            {
                market.cash_rate = vector(*cashRate, "cash_rate");
            }
            if (borrowRate)
            {
                market.borrow_rate = vector(*borrowRate, "borrow_rate");
            }
            market.dividends = dividends;
            market.splits = splits;

            // Costs: a proportional rate, plus half an estimated spread and
            // a square-root impact if asked.
            proportional.emplace(proportionalCost);
            std::vector<const qb::CostModel *> models{&*proportional};
            if (!spreadEstimator.is_none())
            {
                const auto estimator = choose<qb::SpreadEstimator>(py::cast<std::string>(spreadEstimator), "spread_estimator",
                                                                   {{"corwin_schultz", qb::SpreadEstimator::CorwinSchultz}, {"abdi_ranaldo", qb::SpreadEstimator::AbdiRanaldo}});
                spreads = qb::estimated_spreads(market, estimator, spreadWindow, spreadMinPairs);
                spread = std::make_unique<qb::SpreadCost>(qb::MatrixView{spreads.data(), days.size(), market.assets},
                                                          (estimator == qb::SpreadEstimator::CorwinSchultz ? "Corwin and Schultz (2012)" : "Abdi and Ranaldo (2017)") +
                                                              std::string(", estimated over the ") + std::to_string(spreadWindow) + " days before the execution");
                models.push_back(spread.get());
            }
            // The traded amount of each day, whenever volumes are given: the
            // impact cost needs it, and so does the share of volume traded.
            const std::size_t minimum = std::max<std::size_t>(2, impactWindow / 2);
            if (!market.volume.empty())
            {
                dollarVolume = qb::trailing_dollar_volume(market, impactWindow, minimum);
            }
            if (!impactCoefficient.is_none())
            {
                if (dollarVolume.empty())
                {
                    throw qb::Error("'impact_coefficient' needs the 'volume' table");
                }
                volatility = qb::trailing_volatility(market, impactWindow, minimum);
                impact = std::make_unique<qb::SquareRootImpactCost>(qb::MatrixView{volatility.data(), days.size(), market.assets},
                                                                    qb::MatrixView{dollarVolume.data(), days.size(), market.assets},
                                                                    py::cast<double>(impactCoefficient));
                models.push_back(impact.get());
            }
            stack = std::make_unique<qb::CostStack>(models);

            backtest.initial_capital = initialCapital;
            backtest.timing = choose<qb::ExecutionTiming>(timing, "timing",
                                                          {{"next_close", qb::ExecutionTiming::NextClose},
                                                           {"next_open", qb::ExecutionTiming::NextOpen},
                                                           {"same_close", qb::ExecutionTiming::SameClose}});
            backtest.extra_lag_days = extraLagDays;
            backtest.start_index = startIndex;
            backtest.allow_borrowing = allowBorrowing;
            backtest.allow_short = allowShort;
            backtest.whole_shares = wholeShares;
            // A zero rate and nothing else is "no cost": said as such in the
            // conventions, and refused by the engine at the signal's own close.
            backtest.costs = proportionalCost == 0.0 && !spread && !impact ? nullptr : stack.get();
        }
    };

    qb::AllocationConfig toAllocation(const std::string &method, std::size_t lookback, const py::object &rebalance, const std::string &covariance,
                                      bool expanding, double minWeight, double maxWeight, double periodsPerYear)
    {
        qb::AllocationConfig allocation;
        allocation.method = choose<qb::AllocationMethod>(method, "method",
                                                         {{"equal_weight", qb::AllocationMethod::EqualWeight},
                                                          {"minimum_variance", qb::AllocationMethod::MinimumVariance},
                                                          {"maximum_sharpe", qb::AllocationMethod::MaximumSharpe},
                                                          {"risk_parity", qb::AllocationMethod::RiskParity}});
        allocation.covariance = choose<qb::CovarianceEstimator>(covariance, "covariance",
                                                                {{"ledoit_wolf", qb::CovarianceEstimator::LedoitWolf}, {"sample", qb::CovarianceEstimator::Sample}});
        allocation.lookback = lookback;
        allocation.expanding = expanding;
        if (py::isinstance<py::str>(rebalance))
        {
            if (py::cast<std::string>(rebalance) != "monthly")
            {
                throw qb::Error("'rebalance' must be a number of days or 'monthly'");
            }
            allocation.schedule = qb::RebalanceSchedule::FirstDayOfMonth;
        }
        else
        {
            allocation.schedule = qb::RebalanceSchedule::EveryNDays;
            allocation.rebalance_every = py::cast<std::size_t>(rebalance);
        }
        allocation.min_weight = minWeight;
        allocation.max_weight = maxWeight;
        allocation.periods_per_year = periodsPerYear;
        return allocation;
    }

    // One entry of a grid: {"method", "lookback", "rebalance"} and, optional,
    // "covariance", "expanding", "min_weight", "max_weight".
    qb::AllocationConfig toAllocation(const py::dict &entry, double periodsPerYear)
    {
        static const char *const known[] = {"method", "lookback", "rebalance", "covariance", "expanding", "min_weight", "max_weight"};
        for (const auto &item : entry)
        {
            const std::string key = py::cast<std::string>(item.first);
            if (std::find(std::begin(known), std::end(known), key) == std::end(known))
            {
                throw qb::Error("unknown key '" + key + "' in a configuration of the grid");
            }
        }
        for (const char *required : {"method", "lookback", "rebalance"})
        {
            if (!entry.contains(required))
            {
                throw qb::Error(std::string("a configuration of the grid needs the key '") + required + "'");
            }
        }
        return toAllocation(py::cast<std::string>(entry["method"]), py::cast<std::size_t>(entry["lookback"]), entry["rebalance"],
                            entry.contains("covariance") ? py::cast<std::string>(entry["covariance"]) : std::string("ledoit_wolf"),
                            entry.contains("expanding") && py::cast<bool>(entry["expanding"]),
                            entry.contains("min_weight") ? py::cast<double>(entry["min_weight"]) : 0.0,
                            entry.contains("max_weight") ? py::cast<double>(entry["max_weight"]) : 1.0, periodsPerYear);
    }

    // The parameters are in the order of the py::arg list at the bottom.
    py::dict runAllocationGrid(const py::object &dates, const Array &close, const py::list &grid, double initialCapital, double proportionalCost,
                               const std::optional<Array> &open, const std::optional<Array> &high, const std::optional<Array> &low, const std::optional<Array> &volume,
                               const Members &universe, const std::optional<Array> &cashRate, const std::optional<Array> &borrowRate,
                               const py::object &dividends, const py::object &splits,
                               const std::string &timing, std::size_t extraLagDays, std::size_t startIndex, bool allowBorrowing, bool allowShort, bool wholeShares,
                               const py::object &spreadEstimator, std::size_t spreadWindow, std::size_t spreadMinPairs,
                               const py::object &impactCoefficient, std::size_t impactWindow,
                               double periodsPerYear, std::size_t threads, const Callback &progress, const Callback &cancel)
    {
        const Setup setup(dates, close, open, high, low, volume, universe, cashRate, borrowRate, dividends, splits, initialCapital, proportionalCost,
                          timing, extraLagDays, startIndex, allowBorrowing, allowShort, wholeShares, spreadEstimator, spreadWindow, spreadMinPairs,
                          impactCoefficient, impactWindow);
        std::vector<qb::AllocationConfig> configurations;
        for (const py::handle &entry : grid)
        {
            configurations.push_back(toAllocation(py::cast<py::dict>(entry), periodsPerYear));
        }
        const Control control(progress, cancel);
        qb::GridResult result;
        {
            const py::gil_scoped_release release;
            result = qb::run_allocation_grid(setup.market, configurations, setup.backtest, threads, control.get());
        }
        py::dict out;
        out["dates"] = fromDates(result.dates);
        out["returns"] = toArray(result.returns, result.dates.size(), result.configurations);
        out["descriptions"] = result.descriptions;
        out["final_values"] = toArray(result.final_values);
        return out;
    }

    // The parameters are in the order of the py::arg list at the bottom.
    py::dict runAllocation(const py::object &dates, const Array &close,
                           const std::string &method, std::size_t lookback, const py::object &rebalance, double initialCapital, double proportionalCost,
                           const std::optional<Array> &open, const std::optional<Array> &high, const std::optional<Array> &low, const std::optional<Array> &volume,
                           const Members &universe, const std::optional<Array> &cashRate, const std::optional<Array> &borrowRate,
                           const py::object &dividends, const py::object &splits,
                           const std::string &covariance, bool expanding, double minWeight, double maxWeight,
                           const std::string &timing, std::size_t extraLagDays, std::size_t startIndex, bool allowBorrowing, bool allowShort, bool wholeShares,
                           const py::object &spreadEstimator, std::size_t spreadWindow, std::size_t spreadMinPairs,
                           const py::object &impactCoefficient, std::size_t impactWindow,
                           double periodsPerYear, const Callback &progress, const Callback &cancel)
    {
        const Setup setup(dates, close, open, high, low, volume, universe, cashRate, borrowRate, dividends, splits, initialCapital, proportionalCost,
                          timing, extraLagDays, startIndex, allowBorrowing, allowShort, wholeShares, spreadEstimator, spreadWindow, spreadMinPairs,
                          impactCoefficient, impactWindow);
        const qb::MarketData &market = setup.market;
        const qb::BacktestConfig &backtest = setup.backtest;
        const qb::AllocationConfig allocation = toAllocation(method, lookback, rebalance, covariance, expanding, minWeight, maxWeight, periodsPerYear);

        const Control control(progress, cancel);
        qb::ComparedRuns runs;
        {
            const py::gil_scoped_release release;
            runs = qb::run_with_comparisons(market, allocation, backtest, control.get());
        }

        py::dict conventions;
        const qb::Conventions &c = runs.strategy.conventions;
        conventions["execution"] = c.execution;
        conventions["valuation"] = c.valuation;
        conventions["costs"] = c.costs;
        conventions["cash"] = c.cash;
        conventions["corporate_actions"] = c.corporate_actions;
        conventions["turnover"] = c.turnover;
        conventions["strategy"] = runs.description;

        py::list quality;
        for (const qb::QualityIssue &issue : runs.strategy.quality.issues)
        {
            py::dict item;
            item["severity"] = issue.severity == qb::Severity::Error ? "error" : "warning";
            item["asset"] = issue.asset == qb::npos ? py::object(py::none()) : py::object(py::int_(issue.asset));
            item["count"] = issue.count;
            item["message"] = issue.message;
            quality.append(item);
        }

        const std::size_t rebalancings = runs.allocations.size();
        std::vector<qb::Date> allocationDates(rebalancings);
        std::vector<double> weights;
        std::vector<double> inSample(rebalancings);
        std::vector<double> shrinkage(rebalancings);
        py::array_t<std::uint64_t> eligible(static_cast<py::ssize_t>(rebalancings));
        for (std::size_t k = 0; k < rebalancings; ++k)
        {
            const qb::AllocationRecord &record = runs.allocations[k];
            allocationDates[k] = record.date;
            weights.insert(weights.end(), record.weights.begin(), record.weights.end());
            inSample[k] = record.in_sample_sharpe;
            shrinkage[k] = record.shrinkage;
            eligible.mutable_at(static_cast<py::ssize_t>(k)) = record.eligible;
        }
        py::dict allocations;
        allocations["dates"] = fromDates(allocationDates);
        allocations["weights"] = toArray(weights, rebalancings, market.assets);
        allocations["eligible"] = eligible;
        allocations["in_sample_sharpe"] = toArray(inSample);
        allocations["shrinkage"] = toArray(shrinkage);

        // Section 5.2 of the companion: what trading costs the strategy.
        const qb::CostSummary summary = qb::summarize_costs(runs.strategy, runs.gross ? *runs.gross : runs.strategy, market.cash_rate,
                                                            startIndex, periodsPerYear, 0.95);
        py::dict costSummary;
        costSummary["annualized_turnover"] = summary.annualized_turnover;
        costSummary["annual_cost_bps"] = summary.annual_cost_bps;
        costSummary["break_even_cost"] = summary.break_even_cost ? py::object(toDict(*summary.break_even_cost)) : py::object(py::none());
        if (setup.dollarVolume.empty())
        {
            costSummary["participation"] = py::none();
        }
        else
        {
            // Share of the daily traded amount taken by the executions.
            const qb::Participation share = qb::participation(runs.strategy, {setup.dollarVolume.data(), setup.days.size(), market.assets}, setup.days);
            py::dict participation;
            participation["executions"] = share.executions;
            participation["maximum"] = share.maximum;
            participation["median"] = share.median;
            costSummary["participation"] = participation;
        }

        py::dict out;
        out["strategy"] = toDict(runs.strategy);
        out["equal_weight"] = toDict(runs.equal_weight);
        out["buy_and_hold"] = toDict(runs.buy_and_hold);
        out["gross"] = runs.gross ? py::object(toDict(*runs.gross)) : py::object(py::none());
        out["cost_summary"] = costSummary;
        out["allocations"] = allocations;
        out["conventions"] = conventions;
        out["quality"] = quality;
        return out;
    }

    // ---- Validation: a table of returns, one column per configuration ----

    qb::MatrixView configurations(const Array &returns)
    {
        if (returns.ndim() != 2)
        {
            throw qb::Error("'returns' must be a two-dimensional array, one row per period and one column per configuration tried");
        }
        return {returns.data(), static_cast<std::size_t>(returns.shape(0)), static_cast<std::size_t>(returns.shape(1))};
    }

    py::array_t<std::uint64_t> toIndexArray(const std::vector<std::size_t> &values)
    {
        py::array_t<std::uint64_t> out(static_cast<py::ssize_t>(values.size()));
        for (std::size_t k = 0; k < values.size(); ++k)
        {
            out.mutable_at(static_cast<py::ssize_t>(k)) = values[k];
        }
        return out;
    }

    py::dict walkForward(const Array &returns, std::size_t train, std::size_t test, bool expanding, double periodsPerYear)
    {
        const qb::WalkForwardResult r = qb::walk_forward(configurations(returns), {train, test, expanding, periodsPerYear});
        py::list folds;
        for (const qb::WalkForwardFold &fold : r.folds)
        {
            py::dict item;
            item["train_begin"] = fold.train_begin;
            item["train_end"] = fold.train_end;
            item["test_begin"] = fold.test_begin;
            item["test_end"] = fold.test_end;
            item["chosen"] = fold.chosen;
            item["in_sample_sharpe"] = fold.in_sample_sharpe;
            item["out_of_sample_sharpe"] = fold.out_of_sample_sharpe;
            folds.append(item);
        }
        py::dict out;
        out["folds"] = folds;
        out["first_row"] = r.first_row;
        out["returns"] = toArray(r.returns);
        out["chosen"] = toIndexArray(r.chosen);
        return out;
    }

    py::dict combinatorialPurgedCv(const Array &returns, std::size_t groups, std::size_t testGroups, std::size_t purge, std::size_t embargo, double periodsPerYear)
    {
        qb::CpcvConfig config;
        config.groups = groups;
        config.test_groups = testGroups;
        config.purge = purge;
        config.embargo = embargo;
        config.periods_per_year = periodsPerYear;
        const qb::MatrixView view = configurations(returns);
        const qb::CpcvResult r = qb::combinatorial_purged_cv(view, config);
        py::dict out;
        out["splits"] = r.splits;
        out["paths"] = r.paths;
        out["path_sharpes"] = toArray(r.path_sharpes);
        out["path_returns"] = toArray(r.path_returns, r.paths, view.rows());
        out["in_sample_sharpes"] = toArray(r.in_sample_sharpes);
        out["out_of_sample_sharpes"] = toArray(r.out_of_sample_sharpes);
        out["chosen"] = toIndexArray(r.chosen);
        return out;
    }

    py::dict deflatedSharpe(const Array &returns, double periodsPerYear)
    {
        if (!(periodsPerYear > 0.0))
        {
            throw qb::Error("the number of periods per year must be positive");
        }
        const qb::DeflatedSharpe d = qb::deflated_sharpe_of_best(configurations(returns));
        const double annual = std::sqrt(periodsPerYear);
        py::dict out;
        out["best"] = d.best;
        out["sharpe"] = d.sharpe * annual;
        out["expected_maximum"] = d.expected_maximum * annual;
        out["trials"] = d.trials;
        out["sharpe_deviation"] = d.sharpe_deviation * annual;
        out["deflated_sharpe_ratio"] = d.deflated_sharpe_ratio;
        return out;
    }

    py::dict backtestOverfitting(const Array &returns, std::size_t blocks, double periodsPerYear)
    {
        if (!(periodsPerYear > 0.0))
        {
            throw qb::Error("the number of periods per year must be positive");
        }
        const qb::BacktestOverfitting r = qb::probability_of_backtest_overfitting(configurations(returns), blocks);
        const double annual = std::sqrt(periodsPerYear);
        py::dict out;
        out["probability"] = r.probability;
        out["splits"] = r.splits;
        out["logits"] = toArray(r.logits);
        out["mean_in_sample_sharpe"] = r.mean_in_sample_sharpe * annual;
        out["mean_out_of_sample_sharpe"] = r.mean_out_of_sample_sharpe * annual;
        return out;
    }

#ifdef QB_HAS_HEDGING
    double number(const py::dict &values, const char *key, const char *where)
    {
        if (!values.contains(key))
        {
            throw qb::Error(std::string("'") + where + "' needs the key '" + key + "'");
        }
        return py::cast<double>(values[key]);
    }

    // world: {"model": "black_scholes", "spot", "rate", "dividend_yield", "volatility"}
    //     or {"model": "heston", "spot", "rate", "dividend_yield", "v0", "kappa", "theta", "xi", "rho"}.
    // A model is named and given by its parameters: no object of the pricing
    // wheel crosses (ADR-005).
    qb::HedgingConfig hedgingConfig(const py::dict &world, double strike, double maturity, bool isCall, std::size_t rebalances,
                                    std::size_t paths, std::uint64_t seed, std::size_t threads)
    {
        qb::HedgingConfig config;
        if (!world.contains("model"))
        {
            throw qb::Error("'world' needs the key 'model'");
        }
        config.world.kind = choose<qb::WorldKind>(py::cast<std::string>(world["model"]), "model",
                                                  {{"black_scholes", qb::WorldKind::BlackScholes}, {"heston", qb::WorldKind::Heston}});
        config.world.spot = number(world, "spot", "world");
        config.world.rate = number(world, "rate", "world");
        config.world.dividend_yield = number(world, "dividend_yield", "world");
        if (config.world.kind == qb::WorldKind::BlackScholes)
        {
            config.world.volatility = number(world, "volatility", "world");
        }
        else
        {
            config.world.heston = {number(world, "v0", "world"), number(world, "kappa", "world"), number(world, "theta", "world"),
                                   number(world, "xi", "world"), number(world, "rho", "world")};
            if (world.contains("substeps"))
            {
                config.world.substeps = py::cast<std::size_t>(world["substeps"]);
            }
        }
        config.option = {strike, maturity, isCall};
        config.rebalances = rebalances;
        config.paths = paths;
        config.seed = seed;
        config.threads = threads;
        return config;
    }

    py::dict fitMinimumVarianceDelta(const py::dict &world, double strike, double maturity, std::size_t rebalances, std::size_t paths,
                                     std::uint64_t seed, bool isCall, std::size_t threads, const Callback &progress, const Callback &cancel)
    {
        const qb::HedgingConfig config = hedgingConfig(world, strike, maturity, isCall, rebalances, paths, seed, threads);
        const Control control(progress, cancel);
        qb::MinimumVarianceCoefficients k;
        {
            const py::gil_scoped_release release;
            k = qb::fit_minimum_variance_delta(config, control.get());
        }
        py::dict out;
        out["a"] = k.a;
        out["b"] = k.b;
        out["c"] = k.c;
        return out;
    }

    py::dict hedgingBacktest(const py::dict &world, double strike, double maturity, const std::string &hedge, std::size_t rebalances,
                             std::size_t paths, std::uint64_t seed, bool isCall, const py::object &hedgeVolatility, const py::object &coefficients,
                             double costRate, double confidence, std::size_t threads, const Callback &progress, const Callback &cancel)
    {
        qb::HedgingConfig config = hedgingConfig(world, strike, maturity, isCall, rebalances, paths, seed, threads);
        config.hedge = choose<qb::HedgeKind>(hedge, "hedge",
                                             {{"black_scholes_fixed", qb::HedgeKind::BlackScholesFixed},
                                              {"black_scholes_implied", qb::HedgeKind::BlackScholesImplied},
                                              {"skew_adjusted", qb::HedgeKind::SkewAdjusted},
                                              {"minimum_variance", qb::HedgeKind::MinimumVariance},
                                              {"true_model", qb::HedgeKind::TrueModel}});
        if (config.hedge == qb::HedgeKind::BlackScholesFixed)
        {
            if (hedgeVolatility.is_none())
            {
                throw qb::Error("hedge 'black_scholes_fixed' needs 'hedge_volatility'");
            }
            config.hedge_volatility = py::cast<double>(hedgeVolatility);
        }
        if (config.hedge == qb::HedgeKind::MinimumVariance)
        {
            if (coefficients.is_none())
            {
                throw qb::Error("hedge 'minimum_variance' needs 'coefficients', as returned by fit_minimum_variance_delta()");
            }
            const py::dict k = py::dict(coefficients);
            config.coefficients = {number(k, "a", "coefficients"), number(k, "b", "coefficients"), number(k, "c", "coefficients")};
        }
        config.cost_rate = costRate;
        config.confidence = confidence;

        const Control control(progress, cancel);
        qb::HedgingResult r;
        {
            const py::gil_scoped_release release;
            r = qb::run_hedging_backtest(config, control.get());
        }
        py::dict explanation;
        explanation["delta"] = toDict(r.explanation.delta);
        explanation["gamma"] = toDict(r.explanation.gamma);
        explanation["theta"] = toDict(r.explanation.theta);
        explanation["vega"] = toDict(r.explanation.vega);
        explanation["unexplained"] = toDict(r.explanation.unexplained);
        explanation["unexplained_step_deviation"] = r.explanation.unexplained_step_deviation;
        explanation["option_step_deviation"] = r.explanation.option_step_deviation;

        py::dict out;
        out["premium"] = r.premium;
        out["mean"] = toDict(r.mean);
        out["standard_deviation"] = toDict(r.standard_deviation);
        out["skewness"] = r.skewness;
        out["kurtosis"] = r.kurtosis;
        py::dict quantiles;
        quantiles["0.01"] = r.quantile_01;
        quantiles["0.05"] = r.quantile_05;
        quantiles["0.95"] = r.quantile_95;
        quantiles["0.99"] = r.quantile_99;
        out["quantiles"] = quantiles;
        out["step_error_autocorrelation"] = r.step_error_autocorrelation;
        out["discretisation_noise"] = r.discretisation_noise;
        out["gamma_integral"] = r.gamma_integral ? py::object(toDict(*r.gamma_integral)) : py::object(py::none());
        out["explanation"] = explanation;
        out["pnl"] = toArray(r.pnl);
        out["uncalibrated_steps"] = r.uncalibrated_steps;
        out["convention"] = r.convention;
        return out;
    }
#endif

    // Type, shape and bytes of each array, in order: the same values in
    // another type, another shape or another order are other data.
    std::string fingerprint(const py::args &arrays)
    {
        qb::Fingerprint hash;
        for (const py::handle &item : arrays)
        {
            const py::array array = py::array::ensure(item, py::array::c_style);
            if (!array)
            {
                throw qb::Error("fingerprint() takes numpy arrays");
            }
            const std::string type = py::str(array.dtype());
            if (array.dtype().kind() == 'O')
            {
                throw qb::Error("fingerprint() cannot read an array of Python objects");
            }
            hash.update(static_cast<std::uint64_t>(type.size()));
            hash.update(type.data(), type.size());
            hash.update(static_cast<std::uint64_t>(array.ndim()));
            for (py::ssize_t d = 0; d < array.ndim(); ++d)
            {
                hash.update(static_cast<std::uint64_t>(array.shape(d)));
            }
            hash.update(array.data(), static_cast<std::size_t>(array.nbytes()));
        }
        return hash.hex();
    }

} // namespace

PYBIND11_MODULE(quantbacktest, m)
{
    m.doc() = "quantBacktest: backtesting engine (strategy, hedging, risk models)";

    // An unusable input is a ValueError whose message can be shown as it is.
    py::register_exception<qb::Error>(m, "Error", PyExc_ValueError);
    py::register_exception<qb::Cancelled>(m, "Cancelled", m.attr("Error"));

    const qb::BuildInfo &info = qb::build_info();
    m.attr("__version__") = std::string(info.version);

    m.def(
        "build_info",
        []()
        {
            const qb::BuildInfo &b = qb::build_info();
            py::dict out;
            out["version"] = std::string(b.version);
            out["git_sha"] = std::string(b.git_sha);
            out["quantmodeling_sha"] = std::string(b.quantmodeling_sha);
            return out;
        },
        "What to record to replay a run: the version of the engine, the commit it was\n"
        "built from, and the commit of quantModeling the hedging module prices with.");

    m.def("fingerprint", &fingerprint,
          "SHA-256 of the given numpy arrays (type, shape and bytes, in order), as 64\n"
          "hexadecimal digits: the fingerprint of the data a run was given.");

    m.def("total_returns", &totalReturns,
          py::arg("dates"), py::arg("close"),
          py::kw_only(),
          py::arg("dividends") = py::none(), py::arg("splits") = py::none(),
          "Arithmetic total returns from raw closes and corporate actions, one row per date\n"
          "and one column per asset: s (P[t] + D[t]) / P[t-1] - 1, with s the split ratio\n"
          "and D the dividend taking effect on day t. The first row is NaN, and so is any\n"
          "return whose close or previous close is missing.");

    m.def("period_rates", &periodRates,
          py::arg("dates"), py::arg("annual_rate"),
          py::kw_only(),
          py::arg("basis"),
          "Simple rate of each period from annualised simple rates (as decimals, 0.05 for\n"
          "5 %): out[t] = annual_rate[t-1] x (dates[t] - dates[t-1] in days) / basis, and\n"
          "out[0] = 0. The rate of a period is the one known when it starts. basis is the\n"
          "number of days of the money-market year, 360 for USD.");

    m.def("performance_report", &performanceReport,
          py::arg("returns"), py::arg("cash_rate"), py::arg("benchmark") = py::none(),
          py::kw_only(),
          py::arg("bootstrap_replications"), py::arg("mean_block_length"), py::arg("seed"),
          py::arg("periods_per_year") = 252.0, py::arg("confidence") = 0.95, py::arg("tail") = 0.05,
          py::arg("threads") = 1, py::arg("progress") = py::none(), py::arg("cancel") = py::none(),
          "Performance of a series of arithmetic returns, every figure with its interval.\n\n"
          "returns: portfolio returns per period, net of costs.\n"
          "cash_rate: simple risk-free rate of each period, aligned with returns.\n"
          "benchmark: returns of a total-return benchmark, or None.\n"
          "bootstrap_replications: resamples of the stationary bootstrap; 0 returns only\n"
          "    the metrics with an analytical standard error.\n"
          "mean_block_length: expected block length of the bootstrap, in periods.\n"
          "seed: seed of the bootstrap. Same inputs and seed, same result, whatever threads.\n"
          "progress: called with the completed fraction. cancel: called after each progress\n"
          "    report; a true value stops the run, which raises Cancelled.\n\n"
          "Returns a dict: observations, periods_per_year, years, metrics (name to value,\n"
          "standard_error, lower, upper, confidence, method, convention), omitted (name to\n"
          "reason), t_statistic, probabilistic_sharpe_ratio, minimum_track_record_years.");

    m.def("var_backtest", &varBacktest,
          py::arg("pnl"), py::arg("value_at_risk"),
          py::kw_only(),
          py::arg("tail_probability"),
          "Backtest of a value-at-risk model: realised losses against announced ones.\n\n"
          "pnl: signed P&L of each day, the hypothetical one (yesterday's portfolio held\n"
          "    still). value_at_risk: the VaR announced for that day, a positive loss.\n"
          "tail_probability: 0.01 for a 99 % VaR.\n\n"
          "Returns a dict: observations, exceptions, expected_exceptions, exception_days\n"
          "(0/1 per day), unconditional_coverage (Kupiec), independence and\n"
          "conditional_coverage (Christoffersen), each with statistic, p_value,\n"
          "degrees_of_freedom (NaN when the sample cannot be tested), transitions\n"
          "((n00, n01), (n10, n11)), traffic_light (zone, cumulative_probability, and for\n"
          "250 days at 99 % only, else None: basel_plus_factor, frtb_multiplier).");

    m.def("sharpe_difference", &sharpeDifference,
          py::arg("first"), py::arg("second"),
          py::kw_only(),
          py::arg("bootstrap_replications"), py::arg("mean_block_length"), py::arg("seed"),
          py::arg("periods_per_year") = 252.0, py::arg("confidence") = 0.95,
          py::arg("threads") = 1, py::arg("progress") = py::none(), py::arg("cancel") = py::none(),
          "Test of the difference between the Sharpe ratios of two series of excess returns\n"
          "over the same periods (first minus second, annualised).\n\n"
          "Returns a dict: analytical (Jobson-Korkie with Memmel's correction: value,\n"
          "standard_error, lower, upper, confidence, method, convention), z_statistic,\n"
          "p_value, correlation, and, unless bootstrap_replications is 0 (then None),\n"
          "bootstrap (same shape, percentile interval) and bootstrap_p_value.");

    m.def("run_allocation", &runAllocation,
          py::arg("dates"), py::arg("close"),
          py::kw_only(),
          py::arg("method"), py::arg("lookback"), py::arg("rebalance"), py::arg("initial_capital"), py::arg("proportional_cost"),
          py::arg("open") = py::none(), py::arg("high") = py::none(), py::arg("low") = py::none(), py::arg("volume") = py::none(),
          py::arg("universe") = py::none(), py::arg("cash_rate") = py::none(), py::arg("borrow_rate") = py::none(),
          py::arg("dividends") = py::none(), py::arg("splits") = py::none(),
          py::arg("covariance") = "ledoit_wolf", py::arg("expanding") = false, py::arg("min_weight") = 0.0, py::arg("max_weight") = 1.0,
          py::arg("timing") = "next_close", py::arg("extra_lag_days") = 0, py::arg("start_index") = 0, py::arg("allow_borrowing") = false,
          py::arg("allow_short") = false, py::arg("whole_shares") = false,
          py::arg("spread_estimator") = py::none(), py::arg("spread_window") = 21, py::arg("spread_min_pairs") = 10,
          py::arg("impact_coefficient") = py::none(), py::arg("impact_window") = 21,
          py::arg("periods_per_year") = 252.0, py::arg("progress") = py::none(), py::arg("cancel") = py::none(),
          "Backtest of an allocation strategy on raw daily data, with its comparison points.\n\n"
          "dates: datetime64 array, strictly increasing. close, open, high, low, volume: raw\n"
          "    (unadjusted) tables, one row per date and one column per asset; NaN = no quote.\n"
          "universe: boolean table of index membership. cash_rate: simple rate of each period.\n"
          "dividends: (assets, ex_dates, amounts per share). splits: (assets, dates, ratios).\n"
          "method: equal_weight, minimum_variance, maximum_sharpe or risk_parity.\n"
          "lookback: daily returns used by the estimates (with expanding=True, the history\n"
          "    required before the first decision, the window then growing).\n"
          "rebalance: days, or 'monthly'.\n"
          "proportional_cost: fraction of the amount traded (0.0005 = 5 basis points).\n"
          "timing: next_close, next_open or same_close.\n"
          "spread_estimator: None, corwin_schultz or abdi_ranaldo (needs high and low).\n"
          "impact_coefficient: None, or Y of the square-root impact (needs volume).\n"
          "allow_short: negative weights (min_weight < 0), with borrow_rate, the fee of each\n"
          "    period on the shares borrowed. whole_shares: orders rounded towards zero.\n\n"
          "Returns a dict: strategy, equal_weight, buy_and_hold, and gross (the strategy\n"
          "without costs; None when it had none), each with dates, value, cash,\n"
          "position_pnl, dividends, interest, costs, traded_notional, turnover,\n"
          "stale_positions, shares, journal), allocations (dates, weights, eligible,\n"
          "in_sample_sharpe, shrinkage), cost_summary (annualized_turnover, annual_cost_bps,\n"
          "break_even_cost, participation), conventions, quality.");

#ifdef QB_HAS_HEDGING
    m.def("fit_minimum_variance_delta", &fitMinimumVarianceDelta,
          py::arg("world"),
          py::kw_only(),
          py::arg("strike"), py::arg("maturity"), py::arg("rebalances"), py::arg("paths"), py::arg("seed"),
          py::arg("is_call") = true, py::arg("threads") = 1, py::arg("progress") = py::none(), py::arg("cancel") = py::none(),
          "Coefficients a, b, c of the minimum-variance delta of Hull and White (2017),\n"
          "estimated on simulated paths of the world. Estimate on one seed, hedge on another.");

    m.def("hedging_backtest", &hedgingBacktest,
          py::arg("world"),
          py::kw_only(),
          py::arg("strike"), py::arg("maturity"), py::arg("hedge"), py::arg("rebalances"), py::arg("paths"), py::arg("seed"),
          py::arg("is_call") = true, py::arg("hedge_volatility") = py::none(), py::arg("coefficients") = py::none(),
          py::arg("cost_rate") = 0.0, py::arg("confidence") = 0.95,
          py::arg("threads") = 1, py::arg("progress") = py::none(), py::arg("cancel") = py::none(),
          "Hedging backtest in a simulated world: one option bought and delta-hedged.\n\n"
          "world: dict naming the model the market follows and its parameters:\n"
          "    {'model': 'black_scholes', 'spot', 'rate', 'dividend_yield', 'volatility'} or\n"
          "    {'model': 'heston', 'spot', 'rate', 'dividend_yield', 'v0', 'kappa', 'theta',\n"
          "     'xi', 'rho'}. Rates are continuous, per year; maturity is in years.\n"
          "hedge: black_scholes_fixed (needs hedge_volatility), black_scholes_implied,\n"
          "    skew_adjusted, minimum_variance (needs coefficients) or true_model.\n"
          "rebalances: number of equally spaced rebalancings over the option's life.\n\n"
          "Returns a dict: premium; mean and standard_deviation of the P&L at maturity (each\n"
          "value, standard_error, lower, upper, confidence, method, convention); skewness,\n"
          "kurtosis, quantiles; step_error_autocorrelation; discretisation_noise;\n"
          "gamma_integral (black_scholes_fixed only, else None); explanation (delta, gamma,\n"
          "theta, vega, unexplained); pnl (one value per path); uncalibrated_steps; convention.");
#endif

    m.def("walk_forward", &walkForward,
          py::arg("returns"),
          py::kw_only(),
          py::arg("train"), py::arg("test"),
          py::arg("expanding") = false, py::arg("periods_per_year") = 252.0,
          "Walk-forward selection over a table of returns, one row per period and one\n"
          "column per configuration tried: at each step the configuration with the best\n"
          "Sharpe ratio on the training window is applied to the next `test` periods.\n\n"
          "Returns a dict: folds (train_begin, train_end, test_begin, test_end, chosen,\n"
          "in_sample_sharpe, out_of_sample_sharpe, annualised), first_row, returns (the\n"
          "out-of-sample returns end to end), chosen (configuration held each period).");

    m.def("combinatorial_purged_cv", &combinatorialPurgedCv,
          py::arg("returns"),
          py::kw_only(),
          py::arg("groups"), py::arg("test_groups"), py::arg("purge"), py::arg("embargo"),
          py::arg("periods_per_year") = 252.0,
          "Combinatorial purged cross-validation (Lopez de Prado 2018) over the same table.\n\n"
          "Returns a dict: splits, paths, path_sharpes (annualised, one per rebuilt\n"
          "out-of-sample path), path_returns (paths x periods), in_sample_sharpes,\n"
          "out_of_sample_sharpes and chosen (one per split).");

    m.def("deflated_sharpe", &deflatedSharpe,
          py::arg("returns"),
          py::kw_only(),
          py::arg("periods_per_year") = 252.0,
          "Deflated Sharpe ratio (Bailey and Lopez de Prado 2014) of the best column of the\n"
          "table, every column counting as one trial.\n\n"
          "Returns a dict: best, sharpe, expected_maximum and sharpe_deviation (annualised),\n"
          "trials, deflated_sharpe_ratio (a probability).");

    m.def("backtest_overfitting", &backtestOverfitting,
          py::arg("returns"),
          py::kw_only(),
          py::arg("blocks"),
          py::arg("periods_per_year") = 252.0,
          "Probability of backtest overfitting by combinatorially symmetric cross-validation\n"
          "(Bailey, Borwein, Lopez de Prado and Zhu 2017); `blocks` is even, at most 20.\n\n"
          "Returns a dict: probability, splits, logits (one per split), and the champion's\n"
          "mean_in_sample_sharpe and mean_out_of_sample_sharpe (annualised).");

    m.def("run_allocation_grid", &runAllocationGrid,
          py::arg("dates"), py::arg("close"),
          py::kw_only(),
          py::arg("grid"), py::arg("initial_capital"), py::arg("proportional_cost"),
          py::arg("open") = py::none(), py::arg("high") = py::none(), py::arg("low") = py::none(), py::arg("volume") = py::none(),
          py::arg("universe") = py::none(), py::arg("cash_rate") = py::none(), py::arg("borrow_rate") = py::none(),
          py::arg("dividends") = py::none(), py::arg("splits") = py::none(),
          py::arg("timing") = "next_close", py::arg("extra_lag_days") = 0, py::arg("start_index") = 0, py::arg("allow_borrowing") = false,
          py::arg("allow_short") = false, py::arg("whole_shares") = false,
          py::arg("spread_estimator") = py::none(), py::arg("spread_window") = 21, py::arg("spread_min_pairs") = 10,
          py::arg("impact_coefficient") = py::none(), py::arg("impact_window") = 21,
          py::arg("periods_per_year") = 252.0, py::arg("threads") = 1, py::arg("progress") = py::none(), py::arg("cancel") = py::none(),
          "One backtest per configuration, on the same data, costs and execution rules.\n\n"
          "grid: list of dicts, each with method, lookback, rebalance and, optional,\n"
          "    covariance, expanding, min_weight, max_weight. Every configuration tried\n"
          "    belongs in it: the number of columns is the number of trials.\n"
          "The other arguments are those of run_allocation.\n\n"
          "Returns a dict: dates, returns (one row per date, one column per configuration:\n"
          "the table walk_forward, combinatorial_purged_cv, deflated_sharpe and\n"
          "backtest_overfitting take), descriptions, final_values.");

    m.def("factor_regression", &factorRegression,
          py::arg("excess"), py::arg("factors"),
          py::kw_only(),
          py::arg("newey_west_lags"),
          py::arg("periods_per_year") = 252.0, py::arg("confidence") = 0.95,
          "Regression of excess returns on factor returns (one row per period, one column\n"
          "per factor), with Newey-West (1987) standard errors.\n\n"
          "Returns a dict: alpha (annualised: value, standard_error, lower, upper,\n"
          "confidence, method, convention), betas (one such dict per factor),\n"
          "alpha_t_statistic, beta_t_statistics, r_squared, residual_volatility\n"
          "(annualised), observations, newey_west_lags.");
}
