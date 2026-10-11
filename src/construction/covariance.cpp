#include "quantBacktest/construction/covariance.hpp"

#include "quantBacktest/core/error.hpp"

#include <Eigen/Dense>

#include <algorithm>
#include <cmath>
#include <string>

namespace quantBacktest
{

    namespace
    {

        using Matrix = Eigen::Matrix<Real, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>;

        // The returns with the mean of each column removed.
        Matrix centred(MatrixView returns, Index minimumPeriods)
        {
            if (returns.empty() || returns.cols() == 0 || returns.rows() < minimumPeriods)
            {
                throw Error("a covariance needs at least " + std::to_string(minimumPeriods) + " periods of returns and one asset");
            }
            const Eigen::Map<const Matrix> x(returns.data(), static_cast<Eigen::Index>(returns.rows()), static_cast<Eigen::Index>(returns.cols()));
            if (!x.allFinite())
            {
                throw Error("a covariance cannot be estimated from missing or infinite returns");
            }
            return x.rowwise() - x.colwise().mean();
        }

        Covariance pack(const Matrix &sigma, Real shrinkage)
        {
            Covariance out;
            out.assets = static_cast<Index>(sigma.rows());
            out.matrix.assign(sigma.data(), sigma.data() + sigma.size());
            out.shrinkage = shrinkage;
            return out;
        }

    } // namespace

    std::vector<Real> mean_returns(MatrixView returns)
    {
        if (returns.empty() || returns.rows() == 0)
        {
            throw Error("mean returns need at least one period");
        }
        std::vector<Real> mean(returns.cols(), 0.0);
        for (Index t = 0; t < returns.rows(); ++t)
        {
            for (Index i = 0; i < returns.cols(); ++i)
            {
                mean[i] += returns(t, i);
            }
        }
        for (Real &m : mean)
        {
            m /= static_cast<Real>(returns.rows());
        }
        return mean;
    }

    Covariance sample_covariance(MatrixView returns)
    {
        const Matrix x = centred(returns, 2);
        const Matrix sigma = (x.transpose() * x) / static_cast<Real>(x.rows() - 1);
        return pack(sigma, 0.0);
    }

    Covariance ledoit_wolf_covariance(MatrixView returns)
    {
        const Matrix x = centred(returns, 2);
        const Real periods = static_cast<Real>(x.rows());
        const Real assets = static_cast<Real>(x.cols());
        const Matrix s = (x.transpose() * x) / periods;
        const Real m = s.trace() / assets;
        const Matrix identity = Matrix::Identity(s.rows(), s.cols());
        const Real d2 = (s - m * identity).squaredNorm() / assets;
        if (!(d2 > 0.0))
        {
            // Already a multiple of the identity: nothing to shrink.
            return pack(s, 0.0);
        }
        // ||x x' - S||^2 = (x'x)^2 - 2 x'Sx + ||S||^2, without forming x x'.
        const Real sNorm = s.squaredNorm();
        Real b2 = 0.0;
        for (Eigen::Index t = 0; t < x.rows(); ++t)
        {
            const auto row = x.row(t);
            const Real xx = row.squaredNorm();
            b2 += xx * xx - 2.0 * (row * s * row.transpose()).value() + sNorm;
        }
        b2 = std::min(b2 / (periods * periods) / assets, d2);
        const Real shrinkage = b2 / d2;
        return pack(shrinkage * m * identity + (1.0 - shrinkage) * s, shrinkage);
    }

} // namespace quantBacktest
