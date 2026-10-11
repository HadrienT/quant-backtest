#include "quantBacktest/inference/performance_report.hpp"

#include "quantBacktest/inference/sharpe_inference.hpp"
#include "quantBacktest/metrics/performance.hpp"

#include <array>
#include <cmath>
#include <limits>

namespace quantBacktest
{

    namespace
    {

        constexpr Real kNaN = std::numeric_limits<Real>::quiet_NaN();

        // The metrics computed on each resample, in this order.
        struct MetricSpec
        {
            const char *name;
            const char *convention;
            const char *method;
        };

        constexpr const char *kBootstrap = "percentile interval of the stationary bootstrap (Politis and Romano 1994)";
        constexpr const char *kDrawdownBootstrap =
            "stationary bootstrap (Politis and Romano 1994): distribution of the statistic over resampled paths of the same length";

        constexpr std::array<MetricSpec, 12> kMetrics{{
            {"annualized_return", "geometric: compounded growth to the power of periods per year over periods, minus one", kBootstrap},
            {"annualized_volatility", "standard deviation of the arithmetic returns times the square root of the number of periods per year", kBootstrap},
            {"sharpe_ratio_bootstrap", "mean over standard deviation of the arithmetic excess returns, times the square root of the number of periods per year", kBootstrap},
            {"sortino_ratio", "mean excess return over the downside deviation, times the square root of the number of periods per year", kBootstrap},
            {"max_drawdown", "deepest fall of the compounded value from a previous high, zero or negative; it deepens with the length of the history", kDrawdownBootstrap},
            {"calmar_ratio", "annualized return over the absolute maximum drawdown", kDrawdownBootstrap},
            {"skewness", "third central moment over the 1.5 power of the variance; 0 for a Gaussian", kBootstrap},
            {"kurtosis", "fourth central moment over the squared variance; 3 for a Gaussian", kBootstrap},
            {"value_at_risk", "historical, per period, as a positive loss: minus the tail quantile of the returns", kBootstrap},
            {"expected_shortfall", "historical, per period, as a positive loss: minus the mean of the returns beyond the tail quantile", kBootstrap},
            {"tracking_error", "standard deviation of the return minus the benchmark return, times the square root of the number of periods per year", kBootstrap},
            {"information_ratio_bootstrap", "mean over standard deviation of the return minus the benchmark return, times the square root of the number of periods per year", kBootstrap},
        }};
        constexpr Index kWithoutBenchmark = 10;

        void computeMetrics(std::span<const Real> returns, std::span<const Real> cash, std::span<const Real> benchmark,
                            const PerformanceConfig &config, std::span<Real> out)
        {
            const Real annual = std::sqrt(config.periods_per_year);
            const std::vector<Real> excess = excess_returns(returns, cash);
            out[0] = cagr(returns, config.periods_per_year);
            out[1] = annualized_volatility(returns, config.periods_per_year);
            out[2] = sharpe_ratio(excess) * annual;
            out[3] = sortino_ratio(excess) * annual;
            out[4] = max_drawdown(cumulative_values(returns));
            // calmar_ratio(), without computing its two terms a second time.
            out[5] = out[4] < 0.0 ? out[0] / -out[4] : kNaN;
            out[6] = skewness(returns);
            out[7] = kurtosis(returns);
            // The expected shortfall sorts the tail: its last value is the
            // value-at-risk.
            out[8] = historical_var(returns, config.tail);
            out[9] = historical_expected_shortfall(returns, config.tail);
            if (!benchmark.empty())
            {
                out[10] = tracking_error(returns, benchmark, config.periods_per_year);
                out[11] = information_ratio(returns, benchmark) * annual;
            }
        }

        std::vector<Real> gather(std::span<const Real> series, std::span<const Index> indices)
        {
            std::vector<Real> out(series.empty() ? 0 : indices.size());
            for (Index k = 0; k < out.size(); ++k)
            {
                out[k] = series[indices[k]];
            }
            return out;
        }

    } // namespace

    const Estimate *PerformanceReport::find(const std::string &name) const noexcept
    {
        for (const NamedEstimate &metric : metrics)
        {
            if (metric.name == name)
            {
                return &metric.estimate;
            }
        }
        return nullptr;
    }

    PerformanceReport performance_report(std::span<const Real> returns, std::span<const Real> cash_rate, std::span<const Real> benchmark,
                                         const PerformanceConfig &config, const RunControl &control)
    {
        if (returns.size() < 4)
        {
            throw Error("a performance report needs at least 4 returns");
        }
        if (cash_rate.size() != returns.size())
        {
            throw Error("the risk-free rate must be a series with one rate per return, the rate of each period");
        }
        if (!benchmark.empty() && benchmark.size() != returns.size())
        {
            throw Error("the benchmark must have one return per portfolio return");
        }
        if (!(config.periods_per_year > 0.0))
        {
            throw Error("the number of periods per year must be positive");
        }

        const Index count = benchmark.empty() ? kWithoutBenchmark : kMetrics.size();
        const std::vector<Real> excess = excess_returns(returns, cash_rate);

        PerformanceReport report;
        report.observations = returns.size();
        report.periods_per_year = config.periods_per_year;
        report.years = static_cast<Real>(returns.size()) / config.periods_per_year;

        // ---- Analytical inference on the Sharpe ratio (section 8.1, 8.2) ----
        const Real sharpe = sharpe_ratio(excess);
        if (is_missing(sharpe))
        {
            report.omitted.push_back({"sharpe_ratio", "the excess returns have a mean but no volatility"});
            report.t_statistic = kNaN;
            report.probabilistic_sharpe_ratio = kNaN;
            report.minimum_track_record_years = kNaN;
        }
        else
        {
            const Estimate estimate = sharpe_estimate(excess, config.periods_per_year, config.confidence);
            report.metrics.push_back({"sharpe_ratio", estimate});
            report.t_statistic = estimate.value / estimate.standard_error;
            Real gamma3 = skewness(excess);
            Real gamma4 = kurtosis(excess);
            if (is_missing(gamma3))
            {
                gamma3 = 0.0;
                gamma4 = 3.0;
            }
            report.probabilistic_sharpe_ratio = probabilistic_sharpe_ratio(sharpe, 0.0, gamma3, gamma4, excess.size());
            report.minimum_track_record_years = minimum_track_record_length(sharpe, 0.0, gamma3, gamma4, config.confidence) / config.periods_per_year;
        }
        if (!benchmark.empty())
        {
            std::vector<Real> activeReturns(returns.size());
            for (Index t = 0; t < returns.size(); ++t)
            {
                activeReturns[t] = returns[t] - benchmark[t];
            }
            if (is_missing(sharpe_ratio(activeReturns)))
            {
                report.omitted.push_back({"information_ratio", "the active returns have a mean but no volatility"});
            }
            else
            {
                Estimate estimate = sharpe_estimate(activeReturns, config.periods_per_year, config.confidence);
                estimate.convention = kMetrics[11].convention;
                report.metrics.push_back({"information_ratio", estimate});
            }
        }

        // ---- Bootstrap intervals for every metric (section 8.3) ----
        std::vector<Real> point(count, kNaN);
        computeMetrics(returns, cash_rate, benchmark, config, point);
        if (config.bootstrap.replications == 0)
        {
            for (Index m = 0; m < count; ++m)
            {
                report.omitted.push_back({kMetrics[m].name, "no interval: the bootstrap was not run"});
            }
            return report;
        }
        if (config.bootstrap.replications < 2)
        {
            throw Error("a bootstrap interval needs at least 2 replications");
        }
        const BootstrapStatistic statistic = [&](std::span<const Index> indices, std::span<Real> out)
        {
            const std::vector<Real> r = gather(returns, indices);
            const std::vector<Real> c = gather(cash_rate, indices);
            const std::vector<Real> b = gather(benchmark, indices);
            computeMetrics(r, c, b, config, out);
        };
        const std::vector<Real> table = stationary_bootstrap(returns.size(), count, statistic, config.bootstrap, control);

        std::vector<Real> column(config.bootstrap.replications);
        for (Index m = 0; m < count; ++m)
        {
            if (!std::isfinite(point[m]))
            {
                report.omitted.push_back({kMetrics[m].name, "the metric is not defined on this series"});
                continue;
            }
            for (Index b = 0; b < column.size(); ++b)
            {
                column[b] = table[b * count + m];
            }
            const BootstrapInterval interval = percentile_interval(column, config.confidence);
            if (is_missing(interval.lower))
            {
                report.omitted.push_back({kMetrics[m].name, "no interval: the metric is not defined on some resamples"});
                continue;
            }
            Estimate estimate;
            estimate.value = point[m];
            estimate.standard_error = interval.standard_error;
            estimate.lower = interval.lower;
            estimate.upper = interval.upper;
            estimate.confidence = config.confidence;
            estimate.method = kMetrics[m].method;
            estimate.convention = kMetrics[m].convention;
            report.metrics.push_back({kMetrics[m].name, std::move(estimate)});
        }
        return report;
    }

} // namespace quantBacktest
