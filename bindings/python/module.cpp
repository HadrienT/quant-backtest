#include <pybind11/functional.h>
#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "quantBacktest/attribution/factor_regression.hpp"
#include "quantBacktest/core/build_info.hpp"
#include "quantBacktest/core/error.hpp"
#include "quantBacktest/core/fingerprint.hpp"
#include "quantBacktest/core/run_control.hpp"
#include "quantBacktest/inference/performance_report.hpp"
#include "quantBacktest/risk_models/var_backtest.hpp"

#include <atomic>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
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
