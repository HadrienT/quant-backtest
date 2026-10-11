#include "quantBacktest/metrics/cost_report.hpp"

#include "quantBacktest/metrics/performance.hpp"

#include <cmath>
#include <vector>

namespace quantBacktest
{

    CostSummary summarize_costs(const BacktestResult &net, const BacktestResult &gross, std::span<const Real> cash_rate, Index start_index,
                                Real periods_per_year, Real confidence)
    {
        const Index days = net.value.size();
        if (days < 3 || gross.value.size() != days)
        {
            throw Error("a cost summary needs two runs over the same days, at least 3");
        }
        if (!cash_rate.empty() && cash_rate.size() < start_index + days)
        {
            throw Error("the rate series does not cover the days of the run");
        }
        if (!(periods_per_year > 0.0) || !(confidence > 0.0 && confidence < 1.0))
        {
            throw Error("the number of periods per year must be positive and the confidence level strictly between 0 and 1");
        }
        CostSummary out;
        Real turnover = 0.0;
        Real costs = 0.0;
        for (Index t = 0; t < days; ++t)
        {
            turnover += net.turnover[t];
            costs += net.costs[t] / net.value[t];
        }
        const Real n = static_cast<Real>(days);
        out.annualized_turnover = turnover / n * periods_per_year;
        out.annual_cost_bps = costs / n * periods_per_year * 1e4;

        // Twice the turnover of the gross run: the amount it trades per year,
        // as a fraction of its value.
        Real traded = 0.0;
        for (Index t = 0; t < days; ++t)
        {
            traded += 2.0 * gross.turnover[t];
        }
        traded = traded / n * periods_per_year;
        if (traded > 0.0)
        {
            std::vector<Real> excess = simple_returns(gross.value);
            for (Index t = 0; t < excess.size(); ++t)
            {
                excess[t] -= cash_rate.empty() ? 0.0 : cash_rate[start_index + t + 1];
            }
            const Real z = normal_quantile(0.5 + 0.5 * confidence);
            Estimate estimate;
            estimate.value = mean(excess) * periods_per_year / traded;
            estimate.standard_error = standard_deviation(excess) / std::sqrt(static_cast<Real>(excess.size())) * periods_per_year / traded;
            estimate.lower = estimate.value - z * estimate.standard_error;
            estimate.upper = estimate.value + z * estimate.standard_error;
            estimate.confidence = confidence;
            estimate.method = "standard error of the mean excess return, independent returns, normal interval; the turnover is taken as known";
            estimate.convention = "fraction of the amount traded: annualised mean excess return without costs over the amount traded per year";
            out.break_even_cost = estimate;
        }
        return out;
    }

} // namespace quantBacktest
