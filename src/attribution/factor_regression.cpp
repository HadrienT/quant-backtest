#include "quantBacktest/attribution/factor_regression.hpp"

#include "quantBacktest/core/error.hpp"

#include <Eigen/Dense>

#include <cmath>
#include <limits>
#include <string>

namespace quantBacktest
{

    namespace
    {

        using Matrix = Eigen::MatrixXd;
        using Vector = Eigen::VectorXd;

        void validate(std::span<const Real> excess, const MatrixView &factors)
        {
            if (factors.empty() || factors.cols() == 0)
            {
                throw Error("a factor regression needs at least one factor");
            }
            if (factors.rows() != excess.size())
            {
                throw Error("the factors must have one row per return");
            }
            for (const Real x : excess)
            {
                if (!std::isfinite(x))
                {
                    throw Error("a factor regression cannot use a missing or infinite return");
                }
            }
            for (Index k = 0; k < factors.rows() * factors.cols(); ++k)
            {
                if (!std::isfinite(factors.data()[k]))
                {
                    throw Error("a factor regression cannot use a missing or infinite factor return");
                }
            }
        }

        // Rows [first, first + count): a column of ones, then the factors.
        Matrix design(const MatrixView &factors, Index first, Index count)
        {
            Matrix x(static_cast<Eigen::Index>(count), static_cast<Eigen::Index>(factors.cols() + 1));
            for (Index t = 0; t < count; ++t)
            {
                x(static_cast<Eigen::Index>(t), 0) = 1.0;
                for (Index k = 0; k < factors.cols(); ++k)
                {
                    x(static_cast<Eigen::Index>(t), static_cast<Eigen::Index>(k + 1)) = factors(first + t, k);
                }
            }
            return x;
        }

        // Solves the normal equations; throws when the factors are collinear.
        Vector solve(const Matrix &x, const Vector &y, Matrix *inverse)
        {
            const Matrix gram = x.transpose() * x;
            // Collinearity is judged on the correlations between the columns,
            // not on their scale: a factor in basis points is still a factor.
            const Vector scale = gram.diagonal().cwiseSqrt().cwiseInverse();
            const Eigen::LDLT<Matrix> scaled(scale.asDiagonal() * gram * scale.asDiagonal());
            const Vector pivots = scaled.vectorD();
            if (!scale.allFinite() || scaled.info() != Eigen::Success || !(pivots.minCoeff() > 1e-10 * pivots.maxCoeff()))
            {
                throw Error("the factors are collinear, or one of them is constant: the regression has no unique solution");
            }
            const Eigen::LDLT<Matrix> factorisation(gram);
            if (inverse != nullptr)
            {
                *inverse = factorisation.solve(Matrix::Identity(gram.rows(), gram.cols()));
            }
            return factorisation.solve(x.transpose() * y);
        }

        Estimate estimate(Real value, Real error, Real confidence, const std::string &method, const std::string &convention)
        {
            const Real z = normal_quantile(0.5 + 0.5 * confidence);
            Estimate out;
            out.value = value;
            out.standard_error = error;
            out.lower = value - z * error;
            out.upper = value + z * error;
            out.confidence = confidence;
            out.method = method;
            out.convention = convention;
            return out;
        }

    } // namespace

    FactorRegression factor_regression(std::span<const Real> excess, MatrixView factors, const FactorRegressionConfig &config)
    {
        validate(excess, factors);
        const Index observations = excess.size();
        const Index parameters = factors.cols() + 1;
        if (observations <= parameters + 1)
        {
            throw Error("a factor regression needs more observations than parameters");
        }
        if (config.newey_west_lags >= observations)
        {
            throw Error("the number of Newey-West lags must be below the number of observations");
        }
        if (!(config.periods_per_year > 0.0) || !(config.confidence > 0.0 && config.confidence < 1.0))
        {
            throw Error("the number of periods per year must be positive and the confidence level strictly between 0 and 1");
        }

        const Matrix x = design(factors, 0, observations);
        const Vector y = Eigen::Map<const Vector>(excess.data(), static_cast<Eigen::Index>(observations));
        Matrix inverse;
        const Vector coefficients = solve(x, y, &inverse);
        const Vector residuals = y - x * coefficients;

        // Newey and West (1987), Bartlett weights.
        const Matrix scores = x.array().colwise() * residuals.array();
        Matrix s = scores.transpose() * scores;
        const Eigen::Index rows = scores.rows();
        for (Index lag = 1; lag <= config.newey_west_lags; ++lag)
        {
            const Eigen::Index l = static_cast<Eigen::Index>(lag);
            const Matrix gamma = scores.bottomRows(rows - l).transpose() * scores.topRows(rows - l);
            const Real weight = 1.0 - static_cast<Real>(lag) / static_cast<Real>(config.newey_west_lags + 1);
            s += weight * (gamma + gamma.transpose());
        }
        const Matrix covariance = inverse * s * inverse;

        const std::string method = config.newey_west_lags == 0
                                       ? "ordinary least squares, White heteroskedasticity-consistent standard error, normal interval"
                                       : "ordinary least squares, Newey-West (1987) standard error with " + std::to_string(config.newey_west_lags) + " lags, normal interval";

        FactorRegression out;
        out.observations = observations;
        out.newey_west_lags = config.newey_west_lags;
        const Real alphaError = std::sqrt(covariance(0, 0));
        out.alpha = estimate(coefficients(0) * config.periods_per_year, alphaError * config.periods_per_year, config.confidence, method,
                             "intercept of the regression of the excess returns on the factors, times the number of periods per year");
        out.alpha_t_statistic = coefficients(0) / alphaError;
        for (Index k = 1; k < parameters; ++k)
        {
            const Eigen::Index j = static_cast<Eigen::Index>(k);
            const Real error = std::sqrt(covariance(j, j));
            out.betas.push_back(estimate(coefficients(j), error, config.confidence, method, "slope of the excess returns on the factor, the other factors held fixed"));
            out.beta_t_statistics.push_back(coefficients(j) / error);
        }
        const Real total = (y.array() - y.mean()).square().sum();
        const Real unexplained = residuals.squaredNorm();
        out.r_squared = total > 0.0 ? 1.0 - unexplained / total : std::numeric_limits<Real>::quiet_NaN();
        out.residual_volatility = std::sqrt(unexplained / static_cast<Real>(observations - parameters) * config.periods_per_year);
        return out;
    }

    std::vector<Real> rolling_betas(std::span<const Real> excess, MatrixView factors, Index window)
    {
        validate(excess, factors);
        const Index observations = excess.size();
        const Index count = factors.cols();
        if (window <= count + 2 || window > observations)
        {
            throw Error("the rolling window must hold more observations than parameters and fit in the history");
        }
        std::vector<Real> betas(observations * count, std::numeric_limits<Real>::quiet_NaN());
        for (Index t = window - 1; t < observations; ++t)
        {
            const Index first = t + 1 - window;
            const Matrix x = design(factors, first, window);
            const Vector y = Eigen::Map<const Vector>(excess.data() + first, static_cast<Eigen::Index>(window));
            const Vector coefficients = solve(x, y, nullptr);
            for (Index k = 0; k < count; ++k)
            {
                betas[t * count + k] = coefficients(static_cast<Eigen::Index>(k + 1));
            }
        }
        return betas;
    }

} // namespace quantBacktest
