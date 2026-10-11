#include "quantBacktest/core/estimate.hpp"

#include "quantBacktest/core/error.hpp"

#include <cmath>
#include <numbers>

namespace quantBacktest
{

    Real normal_cdf(Real x) noexcept
    {
        return 0.5 * std::erfc(-x / std::numbers::sqrt2);
    }

    Real normal_quantile(Real p)
    {
        if (!(p > 0.0 && p < 1.0))
        {
            throw Error("a probability must be strictly between 0 and 1");
        }
        // Bisection on the distribution function: slow next to a rational
        // approximation, exact to the last bit that matters, and called a
        // handful of times per run.
        Real low = -40.0;
        Real high = 40.0;
        for (int iteration = 0; iteration < 200; ++iteration)
        {
            const Real middle = 0.5 * (low + high);
            (normal_cdf(middle) < p ? low : high) = middle;
        }
        return 0.5 * (low + high);
    }

} // namespace quantBacktest
