#ifndef QUANTBACKTEST_INFERENCE_PERFORMANCE_REPORT_HPP
#define QUANTBACKTEST_INFERENCE_PERFORMANCE_REPORT_HPP

#include "quantBacktest/core/estimate.hpp"
#include "quantBacktest/core/run_control.hpp"
#include "quantBacktest/core/types.hpp"
#include "quantBacktest/inference/bootstrap.hpp"

#include <span>
#include <string>
#include <vector>

namespace quantBacktest
{

    struct PerformanceConfig
    {
        /// 252 for daily returns.
        Real periods_per_year = 252.0;
        /// Level of every interval.
        Real confidence = 0.95;
        /// Tail probability of the value-at-risk and expected shortfall.
        Real tail = 0.05;
        /// Zero replications: only the metrics with an analytical standard
        /// error are returned.
        BootstrapConfig bootstrap;
    };

    struct NamedEstimate
    {
        std::string name;
        Estimate estimate;
    };

    /// A metric that is not in the report, and why.
    struct OmittedMetric
    {
        std::string name;
        std::string reason;
    };

    /// The performance of a series of returns: every figure with its interval
    /// (findings 10 and 11 of the companion), or absent with the reason.
    struct PerformanceReport
    {
        /// Number of returns, the frequency they were annualised with, and
        /// the length of the history in years.
        Index observations = 0;
        Real periods_per_year = 0.0;
        Real years = 0.0;

        /// Names: annualized_return, annualized_volatility, sharpe_ratio,
        /// sharpe_ratio_bootstrap, sortino_ratio, max_drawdown, calmar_ratio,
        /// skewness, kurtosis, value_at_risk, expected_shortfall, and with a
        /// benchmark tracking_error, information_ratio and
        /// information_ratio_bootstrap. The two Sharpe ratios and the two
        /// information ratios are the same value with two intervals: the
        /// analytical one, and the bootstrap one.
        std::vector<NamedEstimate> metrics;
        std::vector<OmittedMetric> omitted;

        /// t statistic of the mean excess return: the per-period Sharpe ratio
        /// over its standard error.
        Real t_statistic = 0.0;
        /// Probability that the true Sharpe ratio is positive (PSR at a
        /// threshold of zero), and the history, in years, needed to say so at
        /// the confidence level of the report.
        Real probabilistic_sharpe_ratio = 0.0;
        Real minimum_track_record_years = 0.0;

        /// Null if the metric was omitted.
        const Estimate *find(const std::string &name) const noexcept;
    };

    /// `returns` are the arithmetic returns of the portfolio, net of costs;
    /// `cash_rate` the simple risk-free rate of each period, the one of the
    /// time (finding 3); `benchmark` the returns of a total-return benchmark,
    /// or empty. The three are aligned.
    ///
    /// The Sharpe and information ratios get the standard error of Lo (2002)
    /// and Mertens (2002); every metric also gets a percentile interval from
    /// the stationary bootstrap, where the portfolio, the rate and the
    /// benchmark are resampled together.
    PerformanceReport performance_report(std::span<const Real> returns, std::span<const Real> cash_rate, std::span<const Real> benchmark,
                                         const PerformanceConfig &config, const RunControl &control = {});

} // namespace quantBacktest

#endif
