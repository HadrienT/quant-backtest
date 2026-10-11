#include "quantBacktest/engine/vectorized.hpp"

#include "quantBacktest/core/error.hpp"

#include <string>

namespace quantBacktest
{

    std::vector<Real> vectorized_values(MatrixView returns, MatrixView weights, std::span<const Real> cash_rate, Real initial_capital)
    {
        const Index days = returns.rows();
        const Index assets = returns.cols();
        if (weights.rows() != days || weights.cols() != assets)
        {
            throw Error("returns and weights must have the same shape");
        }
        if (!cash_rate.empty() && cash_rate.size() != days)
        {
            throw Error("the rate series does not have one value per date");
        }
        std::vector<Real> value(days, initial_capital);
        for (Index t = 1; t < days; ++t)
        {
            Real growth = 0.0;
            Real invested = 0.0;
            for (Index i = 0; i < assets; ++i)
            {
                const Real w = weights(t, i);
                if (w == 0.0)
                {
                    continue;
                }
                const Real r = returns(t, i);
                if (is_missing(r) || is_missing(w))
                {
                    throw Error("asset " + std::to_string(i) + " has a weight on day " + std::to_string(t) + " but no return, or a missing weight");
                }
                growth += w * r;
                invested += w;
            }
            if (!cash_rate.empty())
            {
                growth += (1.0 - invested) * cash_rate[t];
            }
            value[t] = value[t - 1] * (1.0 + growth);
        }
        return value;
    }

} // namespace quantBacktest
