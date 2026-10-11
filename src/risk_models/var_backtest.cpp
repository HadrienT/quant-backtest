#include "quantBacktest/risk_models/var_backtest.hpp"

#include "quantBacktest/core/error.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace quantBacktest
{

    namespace
    {

        constexpr Real kNaN = std::numeric_limits<Real>::quiet_NaN();

        // count * ln(probability), with 0 ln 0 = 0.
        Real logTerm(Index count, Real probability)
        {
            return count == 0 ? 0.0 : static_cast<Real>(count) * std::log(probability);
        }

        void requireTail(Real tail_probability)
        {
            if (!(tail_probability > 0.0 && tail_probability < 1.0))
            {
                throw Error("the tail probability of the VaR must be strictly between 0 and 1");
            }
        }

        // Survival function of the chi-square distribution, 1 or 2 degrees of
        // freedom.
        Real chiSquareTail(Real x, int degrees)
        {
            return degrees == 1 ? std::erfc(std::sqrt(0.5 * x)) : std::exp(-0.5 * x);
        }

        LikelihoodRatio ratio(Real statistic, int degrees)
        {
            // A ratio is not negative; rounding can leave -1e-16.
            const Real clipped = std::max(statistic, 0.0);
            return {clipped, chiSquareTail(clipped, degrees), degrees};
        }

        Real binomialCumulative(Index upTo, Index trials, Real probability)
        {
            Real total = 0.0;
            const Real n = static_cast<Real>(trials);
            for (Index k = 0; k <= upTo && k <= trials; ++k)
            {
                const Real j = static_cast<Real>(k);
                const Real logChoose = std::lgamma(n + 1.0) - std::lgamma(j + 1.0) - std::lgamma(n - j + 1.0);
                total += std::exp(logChoose + j * std::log(probability) + (n - j) * std::log1p(-probability));
            }
            return std::min(total, 1.0);
        }

    } // namespace

    std::vector<std::uint8_t> var_exceptions(std::span<const Real> pnl, std::span<const Real> value_at_risk)
    {
        if (pnl.size() != value_at_risk.size())
        {
            throw Error("the P&L and the VaR must have the same length: one VaR per day of P&L");
        }
        std::vector<std::uint8_t> exceptions(pnl.size());
        for (Index t = 0; t < pnl.size(); ++t)
        {
            if (!std::isfinite(pnl[t]) || !std::isfinite(value_at_risk[t]))
            {
                throw Error("the P&L and the VaR cannot hold a missing or infinite value");
            }
            if (value_at_risk[t] < 0.0)
            {
                throw Error("the VaR is a positive loss: a negative value was given");
            }
            exceptions[t] = pnl[t] < -value_at_risk[t] ? 1 : 0;
        }
        return exceptions;
    }

    CoverageTests var_coverage_tests(std::span<const std::uint8_t> exceptions, Real tail_probability)
    {
        requireTail(tail_probability);
        if (exceptions.size() < 2)
        {
            throw Error("the coverage tests need at least 2 days");
        }
        CoverageTests out;
        const Index T = exceptions.size();
        out.observations = T;
        for (Index t = 0; t < T; ++t)
        {
            const Index state = exceptions[t] != 0 ? 1 : 0;
            out.exceptions += state;
            if (t > 0)
            {
                ++out.transitions[exceptions[t - 1] != 0 ? 1 : 0][state];
            }
        }
        const Index x = out.exceptions;
        out.expected_exceptions = tail_probability * static_cast<Real>(T);

        // Kupiec.
        const Real rate = static_cast<Real>(x) / static_cast<Real>(T);
        const Real logNull = logTerm(T - x, 1.0 - tail_probability) + logTerm(x, tail_probability);
        const Real logObserved = logTerm(T - x, 1.0 - rate) + logTerm(x, rate);
        out.unconditional_coverage = ratio(-2.0 * (logNull - logObserved), 1);

        // Christoffersen.
        const Index n00 = out.transitions[0][0];
        const Index n01 = out.transitions[0][1];
        const Index n10 = out.transitions[1][0];
        const Index n11 = out.transitions[1][1];
        if (x == 0 || x == T || n00 + n01 == 0 || n10 + n11 == 0)
        {
            out.independence = {kNaN, kNaN, 1};
            out.conditional_coverage = {kNaN, kNaN, 2};
            return out;
        }
        const Real pi = static_cast<Real>(n01 + n11) / static_cast<Real>(n00 + n01 + n10 + n11);
        const Real pi01 = static_cast<Real>(n01) / static_cast<Real>(n00 + n01);
        const Real pi11 = static_cast<Real>(n11) / static_cast<Real>(n10 + n11);
        const Real logIndependent = logTerm(n00 + n10, 1.0 - pi) + logTerm(n01 + n11, pi);
        const Real logMarkov = logTerm(n00, 1.0 - pi01) + logTerm(n01, pi01) + logTerm(n10, 1.0 - pi11) + logTerm(n11, pi11);
        out.independence = ratio(-2.0 * (logIndependent - logMarkov), 1);
        out.conditional_coverage = ratio(out.unconditional_coverage.statistic + out.independence.statistic, 2);
        return out;
    }

    TrafficLight traffic_light(Index exceptions, Index observations, Real tail_probability)
    {
        requireTail(tail_probability);
        if (observations == 0 || exceptions > observations)
        {
            throw Error("the number of exceptions cannot exceed the number of days, which must be positive");
        }
        TrafficLight out;
        out.cumulative_probability = binomialCumulative(exceptions, observations, tail_probability);
        out.zone = out.cumulative_probability < 0.95 ? Zone::Green : (out.cumulative_probability < 0.9999 ? Zone::Amber : Zone::Red);

        // The supervisory table, for its own sample only.
        if (observations == 250 && tail_probability == 0.01)
        {
            static constexpr std::array<Real, 5> plus = {0.40, 0.50, 0.65, 0.75, 0.85};
            static constexpr std::array<Real, 5> frtb = {1.70, 1.76, 1.83, 1.88, 1.92};
            if (exceptions <= 4)
            {
                out.basel_plus_factor = 0.0;
                out.frtb_multiplier = 1.50;
            }
            else if (exceptions <= 9)
            {
                out.basel_plus_factor = plus[exceptions - 5];
                out.frtb_multiplier = frtb[exceptions - 5];
            }
            else
            {
                out.basel_plus_factor = 1.00;
                out.frtb_multiplier = 2.00;
            }
        }
        return out;
    }

    std::string zone_name(Zone zone)
    {
        switch (zone)
        {
            case Zone::Green:
                return "green";
            case Zone::Amber:
                return "amber";
            case Zone::Red:
                return "red";
        }
        return "";
    }

} // namespace quantBacktest
