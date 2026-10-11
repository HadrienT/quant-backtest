#include "quantBacktest/construction/optimizer.hpp"

#include "quantBacktest/core/error.hpp"

#include <Eigen/Dense>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <string>

namespace quantBacktest
{

    namespace
    {

        using Matrix = Eigen::MatrixXd;
        using Vector = Eigen::VectorXd;

        Matrix toMatrix(MatrixView covariance)
        {
            if (covariance.empty() || covariance.rows() == 0 || covariance.rows() != covariance.cols())
            {
                throw Error("the covariance must be a square matrix with one row per asset");
            }
            const Eigen::Index n = static_cast<Eigen::Index>(covariance.rows());
            Matrix sigma(n, n);
            Real scale = 0.0;
            for (Index i = 0; i < covariance.rows(); ++i)
            {
                for (Index j = 0; j < covariance.cols(); ++j)
                {
                    const Real value = covariance(i, j);
                    if (!std::isfinite(value))
                    {
                        throw Error("the covariance holds a missing or infinite value");
                    }
                    sigma(static_cast<Eigen::Index>(i), static_cast<Eigen::Index>(j)) = value;
                    scale = std::max(scale, std::abs(value));
                }
            }
            for (Eigen::Index i = 0; i < n; ++i)
            {
                for (Eigen::Index j = 0; j < i; ++j)
                {
                    if (std::abs(sigma(i, j) - sigma(j, i)) > 1e-9 * scale)
                    {
                        throw Error("the covariance is not symmetric");
                    }
                }
            }
            if (Eigen::LLT<Matrix>(sigma).info() != Eigen::Success)
            {
                throw Error("the covariance is not positive definite: with no more periods than assets, use the shrunk estimator");
            }
            return sigma;
        }

        Vector toVector(std::span<const Real> x, Index size, const char *name)
        {
            if (x.size() != size)
            {
                throw Error(std::string("'") + name + "' must have one value per asset");
            }
            const Vector out = Eigen::Map<const Vector>(x.data(), static_cast<Eigen::Index>(size));
            if (!out.allFinite())
            {
                throw Error(std::string("'") + name + "' holds a missing or infinite value");
            }
            return out;
        }

        std::vector<Real> toStd(const Vector &x)
        {
            return {x.data(), x.data() + x.size()};
        }

        void requireFeasible(const Vector &lower, const Vector &upper)
        {
            if ((lower.array() > upper.array()).any())
            {
                throw Error("infeasible: a lower bound is above its upper bound");
            }
            if (lower.sum() > 1.0 + 1e-12)
            {
                throw Error("infeasible: the lower bounds add up to more than 100 %");
            }
            if (upper.sum() < 1.0 - 1e-12)
            {
                throw Error("infeasible: the upper bounds add up to less than 100 %; with a cap per asset, the cap times the number of assets must reach 100 %");
            }
        }

        enum class State
        {
            Free,
            Lower,
            Upper,
        };

        // A portfolio on the bounds, all weights at their lower bound and then
        // filled in order up to the budget: at most one weight is strictly
        // inside.
        Vector vertex(const Vector &lower, const Vector &upper)
        {
            Vector w = lower;
            Real rest = 1.0 - lower.sum();
            for (Eigen::Index i = 0; i < w.size() && rest > 0.0; ++i)
            {
                const Real add = std::min(upper(i) - lower(i), rest);
                w(i) += add;
                rest -= add;
            }
            return w;
        }

        // min (1/2) w' S w - c' w  subject to  sum(w) = 1,  lower <= w <= upper.
        //
        // Primal active-set method (Nocedal and Wright, "Numerical
        // Optimization", 2nd ed., 2006, section 16.5): the weights at a bound
        // are held there, the others solve the equality-constrained problem; a
        // step stops at the first bound it meets, and a bound whose multiplier
        // has the wrong sign is released. S is positive definite.
        Vector solveQp(const Matrix &s, const Vector &c, const Vector &lower, const Vector &upper, const Vector *start)
        {
            const Eigen::Index n = s.rows();
            requireFeasible(lower, upper);
            Vector w = vertex(lower, upper);
            if (start != nullptr)
            {
                const bool admissible = std::abs(start->sum() - 1.0) <= 1e-9 &&
                                        ((start->array() >= lower.array() - 1e-12) && (start->array() <= upper.array() + 1e-12)).all();
                if (admissible)
                {
                    w = start->cwiseMax(lower).cwiseMin(upper);
                }
            }
            std::vector<State> state(static_cast<std::size_t>(n));
            for (Eigen::Index i = 0; i < n; ++i)
            {
                state[static_cast<std::size_t>(i)] = w(i) <= lower(i) ? State::Lower : (w(i) >= upper(i) ? State::Upper : State::Free);
            }
            const auto movable = [&](Eigen::Index i)
            { return upper(i) > lower(i); };

            const int maxIterations = 200 * static_cast<int>(n) + 1000;
            std::vector<Eigen::Index> freeSet;
            for (int iteration = 0; iteration < maxIterations; ++iteration)
            {
                freeSet.clear();
                for (Eigen::Index i = 0; i < n; ++i)
                {
                    if (state[static_cast<std::size_t>(i)] == State::Free)
                    {
                        freeSet.push_back(i);
                    }
                }
                const Eigen::Index f = static_cast<Eigen::Index>(freeSet.size());
                const Vector gradient = s * w - c;
                const Real scale = 1.0 + gradient.cwiseAbs().maxCoeff();
                Real multiplier = 0.0;

                if (f > 0)
                {
                    // The equality-constrained problem on the free weights.
                    Matrix sFree(f, f);
                    Vector rhs(f);
                    Vector current(f);
                    for (Eigen::Index a = 0; a < f; ++a)
                    {
                        const Eigen::Index i = freeSet[static_cast<std::size_t>(a)];
                        current(a) = w(i);
                        // c_F - S_FB w_B = -(gradient_F - S_FF w_F)
                        rhs(a) = -gradient(i);
                        for (Eigen::Index b = 0; b < f; ++b)
                        {
                            sFree(a, b) = s(i, freeSet[static_cast<std::size_t>(b)]);
                        }
                    }
                    rhs += sFree * current;
                    const Eigen::LLT<Matrix> factor(sFree);
                    const Vector a = factor.solve(rhs);
                    const Vector b = factor.solve(Vector::Ones(f));
                    multiplier = (current.sum() - a.sum()) / b.sum();
                    const Vector target = a + multiplier * b;
                    const Vector step = target - current;

                    if (step.cwiseAbs().maxCoeff() > 1e-13 * (1.0 + current.cwiseAbs().maxCoeff()))
                    {
                        Real length = 1.0;
                        Eigen::Index blocking = -1;
                        for (Eigen::Index k = 0; k < f; ++k)
                        {
                            const Eigen::Index i = freeSet[static_cast<std::size_t>(k)];
                            Real limit = std::numeric_limits<Real>::infinity();
                            if (step(k) < 0.0)
                            {
                                limit = (lower(i) - w(i)) / step(k);
                            }
                            else if (step(k) > 0.0)
                            {
                                limit = (upper(i) - w(i)) / step(k);
                            }
                            if (limit < length)
                            {
                                length = std::max(limit, 0.0);
                                blocking = k;
                            }
                        }
                        for (Eigen::Index k = 0; k < f; ++k)
                        {
                            w(freeSet[static_cast<std::size_t>(k)]) = blocking < 0 ? target(k) : current(k) + length * step(k);
                        }
                        if (blocking >= 0)
                        {
                            // Exactly on the bound, not a rounding away from it.
                            const Eigen::Index i = freeSet[static_cast<std::size_t>(blocking)];
                            const bool down = step(blocking) < 0.0;
                            w(i) = down ? lower(i) : upper(i);
                            state[static_cast<std::size_t>(i)] = down ? State::Lower : State::Upper;
                        }
                        continue;
                    }
                }

                // The free weights are optimal for the current bounds: check
                // the multipliers of the bounds. With no free weight, any
                // multiplier of the budget between the gradients of the two
                // sides is admissible.
                Eigen::Index worst = -1;
                Real violation = 1e-10 * scale;
                if (f > 0)
                {
                    for (Eigen::Index i = 0; i < n; ++i)
                    {
                        if (!movable(i) || state[static_cast<std::size_t>(i)] == State::Free)
                        {
                            continue;
                        }
                        const Real reduced = gradient(i) - multiplier;
                        const Real excess = state[static_cast<std::size_t>(i)] == State::Lower ? -reduced : reduced;
                        if (excess > violation)
                        {
                            violation = excess;
                            worst = i;
                        }
                    }
                    if (worst < 0)
                    {
                        return w;
                    }
                    state[static_cast<std::size_t>(worst)] = State::Free;
                    continue;
                }
                Eigen::Index cheapest = -1;
                Eigen::Index dearest = -1;
                for (Eigen::Index i = 0; i < n; ++i)
                {
                    if (!movable(i))
                    {
                        continue;
                    }
                    if (state[static_cast<std::size_t>(i)] == State::Lower && (cheapest < 0 || gradient(i) < gradient(cheapest)))
                    {
                        cheapest = i;
                    }
                    if (state[static_cast<std::size_t>(i)] == State::Upper && (dearest < 0 || gradient(i) > gradient(dearest)))
                    {
                        dearest = i;
                    }
                }
                if (cheapest < 0 || dearest < 0 || gradient(dearest) - gradient(cheapest) <= violation)
                {
                    return w;
                }
                // Moving weight from the dearest to the cheapest lowers the
                // objective: free both.
                state[static_cast<std::size_t>(cheapest)] = State::Free;
                state[static_cast<std::size_t>(dearest)] = State::Free;
            }
            throw Error("the portfolio optimisation did not converge");
        }

        struct Problem
        {
            Matrix sigma;
            Vector lower;
            Vector upper;
        };

        Problem problem(MatrixView covariance, std::span<const Real> lower, std::span<const Real> upper)
        {
            Problem p;
            p.sigma = toMatrix(covariance);
            p.lower = toVector(lower, covariance.rows(), "lower");
            p.upper = toVector(upper, covariance.rows(), "upper");
            return p;
        }

    } // namespace

    std::vector<Real> equal_weights(Index assets)
    {
        if (assets == 0)
        {
            throw Error("an equal-weight portfolio needs at least one asset");
        }
        return std::vector<Real>(assets, 1.0 / static_cast<Real>(assets));
    }

    Real portfolio_variance(MatrixView covariance, std::span<const Real> weights)
    {
        if (covariance.rows() != covariance.cols() || covariance.rows() != weights.size())
        {
            throw Error("the covariance must be square, with one row per weight");
        }
        Real variance = 0.0;
        for (Index i = 0; i < weights.size(); ++i)
        {
            Real row = 0.0;
            for (Index j = 0; j < weights.size(); ++j)
            {
                row += covariance(i, j) * weights[j];
            }
            variance += weights[i] * row;
        }
        return variance;
    }

    std::vector<Real> risk_contributions(MatrixView covariance, std::span<const Real> weights)
    {
        const Real variance = portfolio_variance(covariance, weights);
        if (!(variance > 0.0))
        {
            throw Error("risk contributions are not defined for a portfolio without variance");
        }
        std::vector<Real> contributions(weights.size());
        for (Index i = 0; i < weights.size(); ++i)
        {
            Real row = 0.0;
            for (Index j = 0; j < weights.size(); ++j)
            {
                row += covariance(i, j) * weights[j];
            }
            contributions[i] = weights[i] * row / variance;
        }
        return contributions;
    }

    std::vector<Real> mean_variance(MatrixView covariance, std::span<const Real> linear, std::span<const Real> lower, std::span<const Real> upper,
                                    std::span<const Real> start)
    {
        const Problem p = problem(covariance, lower, upper);
        const Vector c = linear.empty() ? Vector(Vector::Zero(p.sigma.rows())) : toVector(linear, covariance.rows(), "linear");
        if (start.empty())
        {
            return toStd(solveQp(p.sigma, c, p.lower, p.upper, nullptr));
        }
        const Vector from = toVector(start, covariance.rows(), "start");
        return toStd(solveQp(p.sigma, c, p.lower, p.upper, &from));
    }

    std::vector<Real> minimum_variance(MatrixView covariance, std::span<const Real> lower, std::span<const Real> upper)
    {
        return mean_variance(covariance, {}, lower, upper);
    }

    std::vector<Real> maximum_sharpe(MatrixView covariance, std::span<const Real> expected_excess, std::span<const Real> lower, std::span<const Real> upper)
    {
        const Problem p = problem(covariance, lower, upper);
        const Vector mu = toVector(expected_excess, covariance.rows(), "expected_excess");
        requireFeasible(p.lower, p.upper);

        // The highest expected return within the bounds: fill the best assets
        // first.
        std::vector<Eigen::Index> order(static_cast<std::size_t>(mu.size()));
        std::iota(order.begin(), order.end(), Eigen::Index{0});
        std::stable_sort(order.begin(), order.end(), [&](Eigen::Index a, Eigen::Index b)
                         { return mu(a) > mu(b); });
        Vector best = p.lower;
        Real rest = 1.0 - p.lower.sum();
        for (const Eigen::Index i : order)
        {
            const Real add = std::min(p.upper(i) - p.lower(i), rest);
            best(i) += add;
            rest -= add;
        }
        if (!(mu.dot(best) > 0.0))
        {
            throw Error("no admissible portfolio has a positive expected excess return: the maximum Sharpe ratio is not defined");
        }

        // Along the efficient frontier w(k) = argmin (1/2) w'Sw - k mu'w, the
        // Sharpe ratio rises while variance - k x return is positive and
        // falls after: its maximum is the root of that difference in k.
        Vector w = solveQp(p.sigma, Vector::Zero(mu.size()), p.lower, p.upper, nullptr);
        const auto gap = [&](Real k, Vector &weights)
        {
            weights = solveQp(p.sigma, k * mu, p.lower, p.upper, &weights);
            return weights.dot(p.sigma * weights) - k * mu.dot(weights);
        };
        Real low = 0.0;
        Real high = w.dot(p.sigma * w) / mu.dot(best);
        Vector atHigh = w;
        int doublings = 0;
        while (gap(high, atHigh) > 0.0)
        {
            low = high;
            w = atHigh;
            high *= 2.0;
            if (++doublings > 200)
            {
                throw Error("the maximum Sharpe ratio search did not find the end of the efficient frontier");
            }
        }
        for (int iteration = 0; iteration < 200 && high - low > 1e-14 * high; ++iteration)
        {
            const Real middle = 0.5 * (low + high);
            Vector candidate = w;
            if (gap(middle, candidate) > 0.0)
            {
                low = middle;
                w = candidate;
            }
            else
            {
                high = middle;
            }
        }
        Vector result = w;
        gap(0.5 * (low + high), result);
        return toStd(result);
    }

    std::vector<Real> risk_parity(MatrixView covariance)
    {
        const Matrix sigma = toMatrix(covariance);
        const Eigen::Index n = sigma.rows();
        const Real budget = 1.0 / static_cast<Real>(n);

        // Spinu, "An Algorithm for Computing Risk Parity Weights", 2013: the
        // equal-risk portfolio is, up to a scale, the minimiser of the convex
        // function (1/2) y'Sy - (1/n) sum ln y_i. Damped Newton iterations.
        const auto objective = [&](const Vector &y)
        { return 0.5 * y.dot(sigma * y) - budget * y.array().log().sum(); };
        Vector y = sigma.diagonal().cwiseSqrt().cwiseInverse() * std::sqrt(budget);
        for (int iteration = 0; iteration < 200; ++iteration)
        {
            const Vector gradient = sigma * y - budget * y.cwiseInverse();
            if (gradient.cwiseAbs().maxCoeff() <= 1e-14 * (1.0 + (sigma * y).cwiseAbs().maxCoeff()))
            {
                break;
            }
            Matrix hessian = sigma;
            hessian.diagonal() += budget * y.cwiseInverse().cwiseAbs2();
            const Vector step = -Eigen::LLT<Matrix>(hessian).solve(gradient);
            // Stay positive, and decrease.
            Real length = 1.0;
            for (Eigen::Index i = 0; i < n; ++i)
            {
                if (step(i) < 0.0)
                {
                    length = std::min(length, -0.95 * y(i) / step(i));
                }
            }
            const Real value = objective(y);
            const Real slope = gradient.dot(step);
            while (length > 1e-12 && objective(y + length * step) > value + 1e-4 * length * slope)
            {
                length *= 0.5;
            }
            if (length <= 1e-12)
            {
                break;
            }
            y += length * step;
        }
        const Vector w = y / y.sum();
        const Vector contribution = w.cwiseProduct(sigma * w) / w.dot(sigma * w);
        if (!w.allFinite() || (contribution.array() - budget).abs().maxCoeff() > 1e-8)
        {
            throw Error("the risk parity portfolio did not converge");
        }
        return toStd(w);
    }

} // namespace quantBacktest
