#include "quantBacktest/metrics/performance.hpp"

#include "quantBacktest/core/error.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <string>

namespace quantBacktest
{

    namespace
    {

        constexpr Real kNaN = std::numeric_limits<Real>::quiet_NaN();
        // Below this standard deviation per period a series is constant: what
        // is left is rounding, not risk.
        constexpr Real kZeroVolatility = 1e-12;

        void require(std::span<const Real> x, Index minimum, const char *what)
        {
            if (x.size() < minimum)
            {
                throw Error(std::string(what) + " needs at least " + std::to_string(minimum) + " observations");
            }
            for (const Real value : x)
            {
                if (!std::isfinite(value))
                {
                    throw Error(std::string(what) + ": the series holds a missing or infinite value");
                }
            }
        }

        void requireSameLength(std::span<const Real> a, std::span<const Real> b, const char *what)
        {
            if (a.size() != b.size())
            {
                throw Error(std::string(what) + ": the two series must have the same length");
            }
        }

        // Central moment of order 2, 3 or 4, n denominator.
        Real centralMoment(std::span<const Real> x, int order)
        {
            const Real m = mean(x);
            Real sum = 0.0;
            for (const Real value : x)
            {
                const Real d = value - m;
                const Real square = d * d;
                sum += order == 2 ? square : (order == 3 ? square * d : square * square);
            }
            return sum / static_cast<Real>(x.size());
        }

        Index tailCount(Index observations, Real tail)
        {
            if (!(tail > 0.0 && tail < 1.0))
            {
                throw Error("the tail probability must be strictly between 0 and 1");
            }
            const Index k = static_cast<Index>(std::ceil(tail * static_cast<Real>(observations) - 1e-12));
            return std::clamp<Index>(k, 1, observations);
        }

        std::vector<Real> active(std::span<const Real> returns, std::span<const Real> benchmark, const char *what)
        {
            requireSameLength(returns, benchmark, what);
            std::vector<Real> difference(returns.size());
            for (Index t = 0; t < returns.size(); ++t)
            {
                difference[t] = returns[t] - benchmark[t];
            }
            return difference;
        }

    } // namespace

    std::vector<Real> simple_returns(std::span<const Real> values)
    {
        require(values, 2, "returns from values");
        std::vector<Real> returns(values.size() - 1);
        for (Index t = 1; t < values.size(); ++t)
        {
            if (!(values[t - 1] > 0.0))
            {
                throw Error("returns from values: a value is zero or negative");
            }
            returns[t - 1] = values[t] / values[t - 1] - 1.0;
        }
        return returns;
    }

    std::vector<Real> cumulative_values(std::span<const Real> returns)
    {
        require(returns, 0, "cumulative values");
        std::vector<Real> values(returns.size() + 1, 1.0);
        for (Index t = 0; t < returns.size(); ++t)
        {
            values[t + 1] = values[t] * (1.0 + returns[t]);
        }
        return values;
    }

    std::vector<Real> excess_returns(std::span<const Real> returns, std::span<const Real> cash_rate)
    {
        requireSameLength(returns, cash_rate, "excess returns");
        require(returns, 0, "excess returns");
        require(cash_rate, 0, "excess returns");
        std::vector<Real> excess(returns.size());
        for (Index t = 0; t < returns.size(); ++t)
        {
            excess[t] = returns[t] - cash_rate[t];
        }
        return excess;
    }

    Real mean(std::span<const Real> x)
    {
        require(x, 1, "the mean");
        Real sum = 0.0;
        for (const Real value : x)
        {
            sum += value;
        }
        return sum / static_cast<Real>(x.size());
    }

    Real standard_deviation(std::span<const Real> x)
    {
        require(x, 2, "the standard deviation");
        const Real m = mean(x);
        Real sum = 0.0;
        for (const Real value : x)
        {
            sum += (value - m) * (value - m);
        }
        return std::sqrt(sum / static_cast<Real>(x.size() - 1));
    }

    Real skewness(std::span<const Real> x)
    {
        require(x, 3, "the skewness");
        const Real m2 = centralMoment(x, 2);
        return std::sqrt(m2) > kZeroVolatility ? centralMoment(x, 3) / (m2 * std::sqrt(m2)) : kNaN;
    }

    Real kurtosis(std::span<const Real> x)
    {
        require(x, 4, "the kurtosis");
        const Real m2 = centralMoment(x, 2);
        return std::sqrt(m2) > kZeroVolatility ? centralMoment(x, 4) / (m2 * m2) : kNaN;
    }

    Real cagr(std::span<const Real> returns, Real periods_per_year)
    {
        require(returns, 1, "the annualised return");
        Real growth = 1.0;
        for (const Real r : returns)
        {
            if (!(r > -1.0))
            {
                throw Error("the annualised return: a return of -100 % or less wipes the portfolio out");
            }
            growth *= 1.0 + r;
        }
        return std::pow(growth, periods_per_year / static_cast<Real>(returns.size())) - 1.0;
    }

    Real annualized_volatility(std::span<const Real> returns, Real periods_per_year)
    {
        return standard_deviation(returns) * std::sqrt(periods_per_year);
    }

    Real sharpe_ratio(std::span<const Real> excess)
    {
        const Real sigma = standard_deviation(excess);
        const Real mu = mean(excess);
        if (sigma <= kZeroVolatility)
        {
            return std::abs(mu) <= kZeroVolatility ? 0.0 : kNaN;
        }
        return mu / sigma;
    }

    Real sortino_ratio(std::span<const Real> excess)
    {
        require(excess, 2, "the Sortino ratio");
        Real downside = 0.0;
        for (const Real r : excess)
        {
            downside += r < 0.0 ? r * r : 0.0;
        }
        downside = std::sqrt(downside / static_cast<Real>(excess.size()));
        return downside > kZeroVolatility ? mean(excess) / downside : kNaN;
    }

    std::vector<Real> drawdowns(std::span<const Real> values)
    {
        require(values, 1, "drawdowns");
        std::vector<Real> out(values.size());
        Real peak = values.front();
        for (Index t = 0; t < values.size(); ++t)
        {
            peak = std::max(peak, values[t]);
            if (!(peak > 0.0))
            {
                throw Error("drawdowns: a value is zero or negative");
            }
            out[t] = values[t] / peak - 1.0;
        }
        return out;
    }

    Real max_drawdown(std::span<const Real> values)
    {
        const std::vector<Real> series = drawdowns(values);
        return *std::min_element(series.begin(), series.end());
    }

    Index longest_underwater(std::span<const Real> values)
    {
        require(values, 1, "the time under water");
        Real peak = values.front();
        Index current = 0;
        Index longest = 0;
        for (const Real value : values)
        {
            if (value >= peak)
            {
                peak = value;
                current = 0;
            }
            else
            {
                longest = std::max(longest, ++current);
            }
        }
        return longest;
    }

    Real calmar_ratio(std::span<const Real> returns, Real periods_per_year)
    {
        const Real drawdown = max_drawdown(cumulative_values(returns));
        return drawdown < 0.0 ? cagr(returns, periods_per_year) / -drawdown : kNaN;
    }

    Real historical_var(std::span<const Real> returns, Real tail)
    {
        require(returns, 1, "the value-at-risk");
        const Index k = tailCount(returns.size(), tail);
        std::vector<Real> sorted(returns.begin(), returns.end());
        std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(k - 1), sorted.end());
        return -sorted[k - 1];
    }

    Real historical_expected_shortfall(std::span<const Real> returns, Real tail)
    {
        require(returns, 1, "the expected shortfall");
        const Index k = tailCount(returns.size(), tail);
        std::vector<Real> sorted(returns.begin(), returns.end());
        std::partial_sort(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(k), sorted.end());
        Real sum = 0.0;
        for (Index j = 0; j < k; ++j)
        {
            sum += sorted[j];
        }
        return -sum / static_cast<Real>(k);
    }

    Real tracking_error(std::span<const Real> returns, std::span<const Real> benchmark, Real periods_per_year)
    {
        return annualized_volatility(active(returns, benchmark, "the tracking error"), periods_per_year);
    }

    Real information_ratio(std::span<const Real> returns, std::span<const Real> benchmark)
    {
        return sharpe_ratio(active(returns, benchmark, "the information ratio"));
    }

    Real win_rate(std::span<const Real> returns)
    {
        require(returns, 1, "the win rate");
        const auto wins = std::count_if(returns.begin(), returns.end(), [](Real r)
                                        { return r > 0.0; });
        return static_cast<Real>(wins) / static_cast<Real>(returns.size());
    }

    Real gain_loss_ratio(std::span<const Real> returns)
    {
        require(returns, 1, "the gain to loss ratio");
        Real gains = 0.0;
        Real losses = 0.0;
        Index wins = 0;
        Index defeats = 0;
        for (const Real r : returns)
        {
            if (r > 0.0)
            {
                gains += r;
                ++wins;
            }
            else if (r < 0.0)
            {
                losses -= r;
                ++defeats;
            }
        }
        if (wins == 0 || defeats == 0)
        {
            return kNaN;
        }
        return (gains / static_cast<Real>(wins)) / (losses / static_cast<Real>(defeats));
    }

    std::vector<YearReturn> calendar_year_returns(std::span<const Date> dates, std::span<const Real> returns)
    {
        if (dates.size() != returns.size())
        {
            throw Error("returns by calendar year: there must be one date per return");
        }
        require(returns, 0, "returns by calendar year");
        std::vector<YearReturn> years;
        Real growth = 1.0;
        for (Index t = 0; t < returns.size(); ++t)
        {
            const std::chrono::year_month_day ymd{std::chrono::sys_days{std::chrono::days{dates[t]}}};
            const int year = static_cast<int>(ymd.year());
            if (years.empty() || years.back().year != year)
            {
                if (!years.empty() && year < years.back().year)
                {
                    throw Error("returns by calendar year: the dates must be increasing");
                }
                years.push_back({year, 0.0, 0});
                growth = 1.0;
            }
            growth *= 1.0 + returns[t];
            years.back().value = growth - 1.0;
            ++years.back().observations;
        }
        return years;
    }

} // namespace quantBacktest
