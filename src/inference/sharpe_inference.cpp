#include "quantBacktest/inference/sharpe_inference.hpp"

#include "quantBacktest/core/error.hpp"
#include "quantBacktest/metrics/performance.hpp"

#include <cmath>
#include <limits>

namespace quantBacktest
{

    namespace
    {

        // The variance of sqrt(T) times the Sharpe ratio estimator.
        Real varianceTerm(Real sharpe, Real skewness, Real kurtosis)
        {
            return 1.0 - skewness * sharpe + 0.25 * (kurtosis - 1.0) * sharpe * sharpe;
        }

        void requireConfidence(Real confidence)
        {
            if (!(confidence > 0.0 && confidence < 1.0))
            {
                throw Error("the confidence level must be strictly between 0 and 1");
            }
        }

    } // namespace

    Real sharpe_standard_error(Real sharpe, Real skewness, Real kurtosis, Index observations)
    {
        if (observations == 0)
        {
            throw Error("the standard error of a Sharpe ratio needs observations");
        }
        return std::sqrt(varianceTerm(sharpe, skewness, kurtosis) / static_cast<Real>(observations));
    }

    Estimate sharpe_estimate(std::span<const Real> excess, Real periods_per_year, Real confidence)
    {
        requireConfidence(confidence);
        if (!(periods_per_year > 0.0))
        {
            throw Error("the number of periods per year must be positive");
        }
        const Real sharpe = sharpe_ratio(excess);
        if (is_missing(sharpe))
        {
            throw Error("the Sharpe ratio is not defined: the returns have a mean but no volatility");
        }
        const Real gamma3 = skewness(excess);
        const Real gamma4 = kurtosis(excess);
        // A series without volatility has no shape either: Gaussian values.
        const Real error = is_missing(gamma3) ? sharpe_standard_error(sharpe, 0.0, 3.0, excess.size())
                                              : sharpe_standard_error(sharpe, gamma3, gamma4, excess.size());
        const Real annual = std::sqrt(periods_per_year);
        const Real z = normal_quantile(0.5 + 0.5 * confidence);
        Estimate out;
        out.value = sharpe * annual;
        out.standard_error = error * annual;
        out.lower = out.value - z * out.standard_error;
        out.upper = out.value + z * out.standard_error;
        out.confidence = confidence;
        out.method = "Lo (2002) standard error with the Mertens (2002) correction for skewness and kurtosis, independent returns, normal interval";
        out.convention = "mean over standard deviation of the arithmetic excess returns, times the square root of the number of periods per year";
        return out;
    }

    Real probabilistic_sharpe_ratio(Real sharpe, Real threshold, Real skewness, Real kurtosis, Index observations)
    {
        if (observations < 2)
        {
            throw Error("the probabilistic Sharpe ratio needs at least 2 observations");
        }
        const Real variance = varianceTerm(sharpe, skewness, kurtosis);
        return normal_cdf((sharpe - threshold) * std::sqrt(static_cast<Real>(observations - 1)) / std::sqrt(variance));
    }

    Real minimum_track_record_length(Real sharpe, Real threshold, Real skewness, Real kurtosis, Real confidence)
    {
        requireConfidence(confidence);
        if (!(sharpe > threshold))
        {
            return std::numeric_limits<Real>::infinity();
        }
        const Real z = normal_quantile(confidence);
        const Real ratio = z / (sharpe - threshold);
        return 1.0 + varianceTerm(sharpe, skewness, kurtosis) * ratio * ratio;
    }

} // namespace quantBacktest
