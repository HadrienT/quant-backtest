#include "quantBacktest/risk_models/pla_test.hpp"

#include "quantBacktest/core/error.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <vector>

namespace quantBacktest
{

    namespace
    {

        // Ranks from 1 for the lowest value; equal values share the mean of
        // the ranks they span.
        std::vector<Real> ranks(std::span<const Real> x)
        {
            std::vector<Index> order(x.size());
            std::iota(order.begin(), order.end(), Index{0});
            std::stable_sort(order.begin(), order.end(), [&](Index a, Index b)
                             { return x[a] < x[b]; });
            std::vector<Real> out(x.size());
            for (Index first = 0; first < order.size();)
            {
                Index last = first;
                while (last + 1 < order.size() && x[order[last + 1]] == x[order[first]])
                {
                    ++last;
                }
                const Real shared = 0.5 * static_cast<Real>(first + last) + 1.0;
                for (Index k = first; k <= last; ++k)
                {
                    out[order[k]] = shared;
                }
                first = last + 1;
            }
            return out;
        }

        Real correlation(const std::vector<Real> &a, const std::vector<Real> &b)
        {
            const Real n = static_cast<Real>(a.size());
            const Real meanA = std::accumulate(a.begin(), a.end(), 0.0) / n;
            const Real meanB = std::accumulate(b.begin(), b.end(), 0.0) / n;
            Real cross = 0.0;
            Real squaresA = 0.0;
            Real squaresB = 0.0;
            for (Index k = 0; k < a.size(); ++k)
            {
                cross += (a[k] - meanA) * (b[k] - meanB);
                squaresA += (a[k] - meanA) * (a[k] - meanA);
                squaresB += (b[k] - meanB) * (b[k] - meanB);
            }
            if (!(squaresA > 0.0) || !(squaresB > 0.0))
            {
                throw Error("the P&L attribution test needs two series that are not constant");
            }
            return cross / std::sqrt(squaresA * squaresB);
        }

        // Survival function of the Kolmogorov distribution.
        Real kolmogorovTail(Real lambda)
        {
            if (lambda < 0.2)
            {
                return 1.0;
            }
            Real sum = 0.0;
            for (int j = 1; j <= 100; ++j)
            {
                const Real term = std::exp(-2.0 * static_cast<Real>(j * j) * lambda * lambda);
                sum += (j % 2 == 1 ? term : -term);
                if (term < 1e-16)
                {
                    break;
                }
            }
            return std::clamp(2.0 * sum, 0.0, 1.0);
        }

    } // namespace

    PlaTest pla_test(std::span<const Real> hypothetical, std::span<const Real> risk_theoretical)
    {
        if (hypothetical.size() != risk_theoretical.size())
        {
            throw Error("the two P&L series must cover the same days");
        }
        const Index n = hypothetical.size();
        if (n < 3)
        {
            throw Error("the P&L attribution test needs at least 3 days");
        }
        for (Index t = 0; t < n; ++t)
        {
            if (!std::isfinite(hypothetical[t]) || !std::isfinite(risk_theoretical[t]))
            {
                throw Error("the P&L series cannot hold a missing or infinite value");
            }
        }
        PlaTest out;
        out.observations = n;
        out.spearman = correlation(ranks(hypothetical), ranks(risk_theoretical));

        // The two empirical distribution functions, walked through the merged
        // values; the difference is read after all equal values are passed.
        std::vector<Real> a(hypothetical.begin(), hypothetical.end());
        std::vector<Real> b(risk_theoretical.begin(), risk_theoretical.end());
        std::sort(a.begin(), a.end());
        std::sort(b.begin(), b.end());
        Index i = 0;
        Index j = 0;
        Real largest = 0.0;
        while (i < n || j < n)
        {
            const Real value = j == n || (i < n && a[i] <= b[j]) ? a[i] : b[j];
            while (i < n && a[i] <= value)
            {
                ++i;
            }
            while (j < n && b[j] <= value)
            {
                ++j;
            }
            largest = std::max(largest, std::abs(static_cast<Real>(i) - static_cast<Real>(j)) / static_cast<Real>(n));
        }
        out.kolmogorov_smirnov = largest;
        out.kolmogorov_smirnov_p_value = kolmogorovTail(largest * std::sqrt(0.5 * static_cast<Real>(n)));

        if (n == 250)
        {
            if (out.spearman < 0.70 || out.kolmogorov_smirnov > 0.12)
            {
                out.zone = Zone::Red;
            }
            else if (out.spearman > 0.80 && out.kolmogorov_smirnov < 0.09)
            {
                out.zone = Zone::Green;
            }
            else
            {
                out.zone = Zone::Amber;
            }
        }
        return out;
    }

    DeskBacktest desk_backtest(std::span<const Real> pnl, std::span<const Real> value_at_risk_99, std::span<const Real> value_at_risk_975)
    {
        DeskBacktest out;
        out.observations = pnl.size();
        for (const std::uint8_t exception : var_exceptions(pnl, value_at_risk_99))
        {
            out.exceptions_99 += exception;
        }
        for (const std::uint8_t exception : var_exceptions(pnl, value_at_risk_975))
        {
            out.exceptions_975 += exception;
        }
        out.within_limits = out.exceptions_99 <= 12 && out.exceptions_975 <= 30;
        return out;
    }

} // namespace quantBacktest
