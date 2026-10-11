#ifndef QUANTBACKTEST_CORE_ESTIMATE_HPP
#define QUANTBACKTEST_CORE_ESTIMATE_HPP

#include "quantBacktest/core/types.hpp"

#include <string>

namespace quantBacktest
{

    /// A figure as it is returned: with its uncertainty and with what it
    /// means. A metric that cannot be bracketed is not returned at all
    /// (companion, section 14).
    struct Estimate
    {
        Real value = 0.0;
        Real standard_error = 0.0;
        /// Bounds of the confidence interval at level `confidence`.
        Real lower = 0.0;
        Real upper = 0.0;
        Real confidence = 0.0;
        /// How the uncertainty was obtained. In English, ready to be shown.
        std::string method;
        /// Units, annualisation and definition of the value.
        std::string convention;
    };

    /// Standard normal distribution function, and its inverse for p in (0, 1).
    Real normal_cdf(Real x) noexcept;
    Real normal_quantile(Real p);

} // namespace quantBacktest

#endif
