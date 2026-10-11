#include "quantBacktest/inference/sharpe_difference.hpp"

#include "quantBacktest/metrics/performance.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace quantBacktest
{

    SharpeDifference sharpe_difference(std::span<const Real> first, std::span<const Real> second, Real periods_per_year, Real confidence,
                                       const BootstrapConfig &bootstrap, const RunControl &control)
    {
        if (first.size() != second.size())
        {
            throw Error("the two series of returns must cover the same periods");
        }
        if (first.size() < 4)
        {
            throw Error("comparing two Sharpe ratios needs at least 4 returns");
        }
        if (!(periods_per_year > 0.0) || !(confidence > 0.0 && confidence < 1.0))
        {
            throw Error("the number of periods per year must be positive and the confidence level strictly between 0 and 1");
        }
        const Real sharpeFirst = sharpe_ratio(first);
        const Real sharpeSecond = sharpe_ratio(second);
        const Real deviationFirst = standard_deviation(first);
        const Real deviationSecond = standard_deviation(second);
        if (is_missing(sharpeFirst) || is_missing(sharpeSecond) || !(deviationFirst > 1e-12) || !(deviationSecond > 1e-12))
        {
            throw Error("comparing two Sharpe ratios needs two series with volatility");
        }
        const Index T = first.size();
        const Real meanFirst = mean(first);
        const Real meanSecond = mean(second);
        Real covariance = 0.0;
        for (Index t = 0; t < T; ++t)
        {
            covariance += (first[t] - meanFirst) * (second[t] - meanSecond);
        }
        covariance /= static_cast<Real>(T - 1);
        const Real rho = covariance / (deviationFirst * deviationSecond);

        const Real annual = std::sqrt(periods_per_year);
        const Real variance = (2.0 - 2.0 * rho + 0.5 * (sharpeFirst * sharpeFirst + sharpeSecond * sharpeSecond - 2.0 * sharpeFirst * sharpeSecond * rho * rho)) / static_cast<Real>(T);
        const Real difference = sharpeFirst - sharpeSecond;
        const Real error = std::sqrt(std::max(variance, 0.0));
        const Real z = normal_quantile(0.5 + 0.5 * confidence);
        const char *convention = "annualised Sharpe ratio of the first series minus the one of the second, on the same periods";

        SharpeDifference out;
        out.correlation = rho;
        out.analytical.value = difference * annual;
        out.analytical.standard_error = error * annual;
        out.analytical.lower = (difference - z * error) * annual;
        out.analytical.upper = (difference + z * error) * annual;
        out.analytical.confidence = confidence;
        out.analytical.method = "Jobson and Korkie (1981) with the correction of Memmel (2003): independent Gaussian returns, normal interval";
        out.analytical.convention = convention;
        if (error > 0.0)
        {
            out.z_statistic = difference / error;
            out.p_value = 2.0 * normal_cdf(-std::abs(out.z_statistic));
        }
        else
        {
            // The same series twice: no difference, and nothing to reject.
            out.z_statistic = 0.0;
            out.p_value = 1.0;
        }

        if (bootstrap.replications == 0)
        {
            return out;
        }
        const BootstrapStatistic statistic = [&](std::span<const Index> indices, std::span<Real> result)
        {
            std::vector<Real> a(indices.size());
            std::vector<Real> b(indices.size());
            for (Index k = 0; k < indices.size(); ++k)
            {
                a[k] = first[indices[k]];
                b[k] = second[indices[k]];
            }
            result[0] = (sharpe_ratio(a) - sharpe_ratio(b)) * annual;
        };
        const std::vector<Real> replications = stationary_bootstrap(T, 1, statistic, bootstrap, control);
        const BootstrapInterval interval = percentile_interval(replications, confidence);
        if (is_missing(interval.lower))
        {
            return out;
        }
        Estimate estimate;
        estimate.value = out.analytical.value;
        estimate.standard_error = interval.standard_error;
        estimate.lower = interval.lower;
        estimate.upper = interval.upper;
        estimate.confidence = confidence;
        estimate.method = "percentile interval of the stationary bootstrap (Politis and Romano 1994), the two series resampled together";
        estimate.convention = convention;
        out.bootstrap = estimate;
        const auto below = std::count_if(replications.begin(), replications.end(), [](Real x)
                                         { return x <= 0.0; });
        const auto above = std::count_if(replications.begin(), replications.end(), [](Real x)
                                         { return x >= 0.0; });
        out.bootstrap_p_value = std::min(1.0, 2.0 * static_cast<Real>(std::min(below, above)) / static_cast<Real>(replications.size()));
        return out;
    }

} // namespace quantBacktest
