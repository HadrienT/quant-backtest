#include <gtest/gtest.h>

#include "quantBacktest/attribution/factor_regression.hpp"
#include "quantBacktest/core/error.hpp"

#include <cmath>
#include <limits>
#include <random>
#include <vector>

namespace quantBacktest
{

    namespace
    {

        struct Sample
        {
            std::vector<Real> excess;
            std::vector<Real> factors;
            Index observations = 0;
            Index count = 0;

            MatrixView view() const { return {factors.data(), observations, count}; }
        };

        // Re = alpha + beta . F + e, with errors that are autocorrelated
        // (AR(1) with coefficient `persistence`) and heteroskedastic (their
        // scale follows the size of the first factor).
        Sample simulate(Index observations, Real alpha, const std::vector<Real> &betas, Real persistence, std::mt19937_64 &rng)
        {
            std::normal_distribution<Real> normal(0.0, 1.0);
            Sample s;
            s.observations = observations;
            s.count = betas.size();
            s.factors.resize(observations * s.count);
            s.excess.resize(observations);
            Real error = 0.0;
            for (Index t = 0; t < observations; ++t)
            {
                Real explained = alpha;
                for (Index k = 0; k < s.count; ++k)
                {
                    const Real f = 0.0003 + 0.01 * normal(rng);
                    s.factors[t * s.count + k] = f;
                    explained += betas[k] * f;
                }
                const Real scale = 0.004 * (0.5 + 50.0 * std::abs(s.factors[t * s.count]));
                error = persistence * error + scale * normal(rng);
                s.excess[t] = explained + error;
            }
            return s;
        }

    } // namespace

    TEST(FactorRegression, RecoversAnExactLinearRelation)
    {
        std::mt19937_64 rng(1);
        Sample s = simulate(300, 0.0, {0.0, 0.0}, 0.0, rng);
        for (Index t = 0; t < s.observations; ++t)
        {
            s.excess[t] = 0.0002 + 1.3 * s.factors[t * 2] - 0.4 * s.factors[t * 2 + 1];
        }
        const FactorRegression r = factor_regression(s.excess, s.view(), {});
        EXPECT_NEAR(r.alpha.value, 0.0002 * 252.0, 1e-12);
        ASSERT_EQ(r.betas.size(), 2u);
        EXPECT_NEAR(r.betas[0].value, 1.3, 1e-10);
        EXPECT_NEAR(r.betas[1].value, -0.4, 1e-10);
        EXPECT_NEAR(r.r_squared, 1.0, 1e-12);
        EXPECT_NEAR(r.residual_volatility, 0.0, 1e-10);
        EXPECT_EQ(r.observations, 300u);
    }

    // Finding 4: the alpha is annualised and comes with its standard error;
    // a daily alpha shown to two decimals was "0.00" whatever the result.
    TEST(FactorRegression, AlphaIsAnnualisedWithItsStandardError)
    {
        std::mt19937_64 rng(2);
        const Real dailyAlpha = 0.0004;
        const Sample s = simulate(2520, dailyAlpha, {1.1}, 0.0, rng);
        FactorRegressionConfig config;
        config.newey_west_lags = 5;
        const FactorRegression r = factor_regression(s.excess, s.view(), config);

        // About 10 % a year: a figure that two decimals do not erase.
        EXPECT_NEAR(r.alpha.value, dailyAlpha * 252.0, 3.0 * r.alpha.standard_error);
        EXPECT_GT(std::abs(r.alpha.value), 0.01);
        EXPECT_GT(r.alpha.standard_error, 0.0);
        EXPECT_LT(r.alpha.lower, r.alpha.value);
        EXPECT_GT(r.alpha.upper, r.alpha.value);
        EXPECT_NEAR(r.alpha_t_statistic, r.alpha.value / r.alpha.standard_error, 1e-9);
        EXPECT_NE(r.alpha.convention.find("times the number of periods per year"), std::string::npos);
        EXPECT_NE(r.alpha.method.find("Newey-West (1987) standard error with 5 lags"), std::string::npos);
        EXPECT_EQ(r.newey_west_lags, 5u);
        EXPECT_NEAR(r.betas[0].value, 1.1, 3.0 * r.betas[0].standard_error);

        // Annualisation is a change of unit: it scales value and error alike.
        config.periods_per_year = 1.0;
        const FactorRegression daily = factor_regression(s.excess, s.view(), config);
        EXPECT_NEAR(r.alpha.value, 252.0 * daily.alpha.value, 1e-12);
        EXPECT_NEAR(r.alpha.standard_error, 252.0 * daily.alpha.standard_error, 1e-12);
        EXPECT_NEAR(r.alpha_t_statistic, daily.alpha_t_statistic, 1e-9);
    }

    // Property: with autocorrelated, heteroskedastic errors, the Newey-West
    // interval of the alpha holds the true value about as often as it says,
    // and the interval without lags does not. "About": the Bartlett weights
    // cut the autocovariances short, so the interval is a little too narrow
    // (92 % measured here for 95 % announced, with 12 lags on errors whose
    // autocorrelation is 0.5).
    TEST(FactorRegression, NeweyWestIntervalCoversTheTrueAlpha)
    {
        std::mt19937_64 rng(3);
        const Real dailyAlpha = 0.0002;
        const int draws = 1500;
        int coveredWithLags = 0;
        int coveredWithout = 0;
        for (int draw = 0; draw < draws; ++draw)
        {
            const Sample s = simulate(1000, dailyAlpha, {1.0, 0.3}, 0.5, rng);
            FactorRegressionConfig config;
            config.newey_west_lags = 12;
            const FactorRegression hac = factor_regression(s.excess, s.view(), config);
            config.newey_west_lags = 0;
            const FactorRegression white = factor_regression(s.excess, s.view(), config);
            const Real truth = dailyAlpha * 252.0;
            coveredWithLags += hac.alpha.lower <= truth && truth <= hac.alpha.upper ? 1 : 0;
            coveredWithout += white.alpha.lower <= truth && truth <= white.alpha.upper ? 1 : 0;
        }
        const Real withLags = static_cast<Real>(coveredWithLags) / draws;
        const Real without = static_cast<Real>(coveredWithout) / draws;
        EXPECT_NEAR(withLags, 0.95, 0.04);
        EXPECT_LT(withLags, 0.96);
        EXPECT_LT(without, 0.80);
    }

    // With independent, homoskedastic errors the robust standard error is the
    // classical one.
    TEST(FactorRegression, MatchesTheClassicalStandardErrorOnIdealErrors)
    {
        std::mt19937_64 rng(4);
        std::normal_distribution<Real> normal(0.0, 1.0);
        const Index observations = 20000;
        Sample s;
        s.observations = observations;
        s.count = 1;
        s.factors.resize(observations);
        s.excess.resize(observations);
        for (Index t = 0; t < observations; ++t)
        {
            s.factors[t] = 0.01 * normal(rng);
            s.excess[t] = 0.8 * s.factors[t] + 0.005 * normal(rng);
        }
        const FactorRegression r = factor_regression(s.excess, s.view(), {});
        const Real classicalBetaError = 0.005 / (0.01 * std::sqrt(static_cast<Real>(observations)));
        EXPECT_NEAR(r.betas[0].standard_error, classicalBetaError, 0.05 * classicalBetaError);
        EXPECT_NEAR(r.residual_volatility, 0.005 * std::sqrt(252.0), 0.02 * 0.005 * std::sqrt(252.0));
        EXPECT_NEAR(r.r_squared, 0.64 / (0.64 + 0.25), 0.02);
    }

    TEST(FactorRegression, LagRule)
    {
        EXPECT_EQ(newey_west_lag_rule(100), 4u);
        EXPECT_EQ(newey_west_lag_rule(1000), 6u);
        EXPECT_EQ(newey_west_lag_rule(2520), 8u);
        EXPECT_EQ(newey_west_lag_rule(10), 2u);
        EXPECT_THROW(newey_west_lag_rule(0), Error);
    }

    TEST(FactorRegression, RefusesWhatItCannotSolve)
    {
        std::mt19937_64 rng(5);
        Sample s = simulate(100, 0.0, {1.0, 0.5}, 0.0, rng);
        FactorRegressionConfig config;
        EXPECT_NO_THROW(factor_regression(s.excess, s.view(), config));

        // A factor in basis points is still a factor.
        Sample scaled = s;
        for (Index t = 0; t < s.observations; ++t)
        {
            scaled.factors[t * 2 + 1] *= 1e-6;
        }
        EXPECT_NEAR(factor_regression(scaled.excess, scaled.view(), config).betas[1].value,
                    1e6 * factor_regression(s.excess, s.view(), config).betas[1].value, 1e-3);

        // Two identical factors, then a constant one.
        Sample collinear = s;
        for (Index t = 0; t < s.observations; ++t)
        {
            collinear.factors[t * 2 + 1] = 2.0 * collinear.factors[t * 2];
        }
        EXPECT_THROW(factor_regression(collinear.excess, collinear.view(), config), Error);
        for (Index t = 0; t < s.observations; ++t)
        {
            collinear.factors[t * 2 + 1] = 0.01;
        }
        EXPECT_THROW(factor_regression(collinear.excess, collinear.view(), config), Error);

        s.excess[10] = std::numeric_limits<Real>::quiet_NaN();
        EXPECT_THROW(factor_regression(s.excess, s.view(), config), Error);
        s.excess[10] = 0.0;
        EXPECT_THROW(factor_regression(s.excess, MatrixView{}, config), Error);
        EXPECT_THROW(factor_regression(std::span<const Real>(s.excess).first(50), s.view(), config), Error);
        config.newey_west_lags = 100;
        EXPECT_THROW(factor_regression(s.excess, s.view(), config), Error);
    }

    // A beta that changes through the history shows in the rolling betas and
    // not in the full-sample one.
    TEST(FactorRegression, RollingBetasFollowAChangeOfExposure)
    {
        std::mt19937_64 rng(6);
        std::normal_distribution<Real> normal(0.0, 1.0);
        const Index observations = 1000;
        std::vector<Real> factor(observations);
        std::vector<Real> excess(observations);
        for (Index t = 0; t < observations; ++t)
        {
            factor[t] = 0.01 * normal(rng);
            excess[t] = (t < 500 ? 0.3 : 1.2) * factor[t] + 0.001 * normal(rng);
        }
        const MatrixView view{factor.data(), observations, 1};
        const std::vector<Real> betas = rolling_betas(excess, view, 120);
        ASSERT_EQ(betas.size(), observations);
        EXPECT_TRUE(std::isnan(betas[118]));
        EXPECT_NEAR(betas[119], 0.3, 0.05);
        EXPECT_NEAR(betas[499], 0.3, 0.05);
        EXPECT_NEAR(betas[999], 1.2, 0.05);
        const Real overall = factor_regression(excess, view, {}).betas[0].value;
        EXPECT_GT(overall, 0.5);
        EXPECT_LT(overall, 1.0);

        // Each row is the regression on its own window, and only on it.
        std::vector<Real> window(excess.begin() + 380, excess.begin() + 500);
        const MatrixView windowView{factor.data() + 380, 120, 1};
        EXPECT_NEAR(betas[499], factor_regression(window, windowView, {}).betas[0].value, 1e-12);
        EXPECT_THROW(rolling_betas(excess, view, 2), Error);
        EXPECT_THROW(rolling_betas(excess, view, observations + 1), Error);
    }

} // namespace quantBacktest
