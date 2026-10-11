// The hedging backtest in a simulated world: companion, sections 11 and 16,
// and the acceptance criteria of work package B6.
#include <gtest/gtest.h>

#include "quantBacktest/core/error.hpp"
#include "quantBacktest/hedging/hedging_backtest.hpp"

#include <atomic>
#include <cmath>
#include <numbers>
#include <vector>

namespace quantBacktest
{

    namespace
    {

        HedgingConfig blackScholesWorld(Real realised, Real hedging, Index rebalances, Index paths)
        {
            HedgingConfig c;
            c.world.kind = WorldKind::BlackScholes;
            c.world.spot = 100.0;
            c.world.rate = 0.02;
            c.world.volatility = realised;
            c.option = {100.0, 0.25, true};
            c.hedge = HedgeKind::BlackScholesFixed;
            c.hedge_volatility = hedging;
            c.rebalances = rebalances;
            c.paths = paths;
            c.seed = 7;
            c.threads = 8;
            return c;
        }

        HedgingConfig hestonWorld(HedgeKind hedge, Index rebalances, Index paths)
        {
            HedgingConfig c;
            c.world.kind = WorldKind::Heston;
            c.world.spot = 100.0;
            c.world.rate = 0.01;
            c.world.heston = {0.04, 2.0, 0.04, 0.5, -0.7};
            c.world.substeps = 4;
            c.option = {100.0, 0.25, true};
            c.hedge = hedge;
            c.rebalances = rebalances;
            c.paths = paths;
            c.seed = 11;
            c.threads = 8;
            return c;
        }

    } // namespace

    // Section 16 and the acceptance criterion: in a Black-Scholes world hedged
    // at the true volatility, the standard deviation of the P&L falls as
    // 1 / sqrt(N). The slope measured in log-log is -1/2, within 0.1.
    TEST(Hedging, DiscreteHedgingErrorFallsAsOneOverSquareRootOfN)
    {
        const std::vector<Index> rebalances = {8, 16, 32, 64, 128, 256};
        std::vector<Real> logN;
        std::vector<Real> logDeviation;
        for (const Index n : rebalances)
        {
            const HedgingResult r = run_hedging_backtest(blackScholesWorld(0.2, 0.2, n, 4000));
            logN.push_back(std::log(static_cast<Real>(n)));
            logDeviation.push_back(std::log(r.standard_deviation.value));
            // The mean is zero: the option is bought at its value.
            EXPECT_NEAR(r.mean.value, 0.0, 4.0 * r.mean.standard_error) << n << " rebalancings";
            // And the level is the one of Derman and Kamal, to 15 %.
            EXPECT_NEAR(r.standard_deviation.value, r.discretisation_noise, 0.15 * r.discretisation_noise) << n << " rebalancings";
        }
        Real meanX = 0.0;
        Real meanY = 0.0;
        for (Index k = 0; k < logN.size(); ++k)
        {
            meanX += logN[k] / static_cast<Real>(logN.size());
            meanY += logDeviation[k] / static_cast<Real>(logN.size());
        }
        Real numerator = 0.0;
        Real denominator = 0.0;
        for (Index k = 0; k < logN.size(); ++k)
        {
            numerator += (logN[k] - meanX) * (logDeviation[k] - meanY);
            denominator += (logN[k] - meanX) * (logN[k] - meanX);
        }
        EXPECT_NEAR(numerator / denominator, -0.5, 0.1);
    }

    // The noise floor quoted in the companion: about 0.89 / sqrt(N) of the
    // price for an option at the money.
    TEST(Hedging, NoiseFloorIsAboutPointEightNineOverSquareRootOfNOfThePrice)
    {
        HedgingConfig c = blackScholesWorld(0.2, 0.2, 21, 10);
        c.world.rate = 0.0;
        c.option = {100.0, 1.0 / 12.0, true};
        const HedgingResult r = run_hedging_backtest(c);
        EXPECT_NEAR(r.discretisation_noise / r.premium, 0.89 / std::sqrt(21.0), 0.01);
    }

    // Section 16 and the acceptance criterion: when the option is bought and
    // hedged at an implied volatility that is not the realised one, the mean
    // P&L is the integral of section 11.1, and its sign is the one of
    // realised minus implied variance.
    TEST(Hedging, MeanPnlIsTheGammaWeightedVarianceGap)
    {
        for (const Real implied : {0.15, 0.25})
        {
            const HedgingResult r = run_hedging_backtest(blackScholesWorld(0.2, implied, 252, 4000));
            ASSERT_TRUE(r.gamma_integral.has_value());
            const Real error = std::sqrt(r.mean.standard_error * r.mean.standard_error + r.gamma_integral->standard_error * r.gamma_integral->standard_error);
            EXPECT_NEAR(r.mean.value, r.gamma_integral->value, 4.0 * error) << "implied " << implied;
            EXPECT_NEAR(r.mean.value, r.gamma_integral->value, 0.05 * std::abs(r.gamma_integral->value)) << "implied " << implied;
            // Bought cheap (implied below realised): the buyer earns.
            EXPECT_EQ(r.mean.value > 0.0, implied < 0.2);
            EXPECT_GT(std::abs(r.mean.value), 10.0 * r.mean.standard_error);
        }
        // Hedged at the realised volatility: nothing on average, and no gap.
        const HedgingResult fair = run_hedging_backtest(blackScholesWorld(0.2, 0.2, 252, 4000));
        EXPECT_NEAR(fair.mean.value, 0.0, 4.0 * fair.mean.standard_error);
        EXPECT_EQ(fair.gamma_integral->value, 0.0);
    }

    // Acceptance criterion: in a world of stochastic volatility, the hedge of
    // the true model has the lowest standard deviation.
    //
    // Measured on 4000 paths of this world (63 rebalancings): Black-Scholes
    // delta at the implied volatility 1.059, skew-adjusted 0.977, Hull-White
    // 0.979, true model 0.973, each with a standard error of 0.012. The true
    // model is the lowest; the two corrected deltas come within half a
    // percent of it, and all three are 8 % below the Black-Scholes delta. This
    // test runs fewer paths: it asks the true model to be clearly below the
    // Black-Scholes delta and no worse than the corrected deltas, to 1 %.
    TEST(Hedging, TheTrueModelHedgesBest)
    {
        const Index rebalances = 42;
        const Index paths = 1200;
        const HedgingResult implied = run_hedging_backtest(hestonWorld(HedgeKind::BlackScholesImplied, rebalances, paths));
        const HedgingResult skew = run_hedging_backtest(hestonWorld(HedgeKind::SkewAdjusted, rebalances, paths));
        const HedgingResult truth = run_hedging_backtest(hestonWorld(HedgeKind::TrueModel, rebalances, paths));

        // The Hull-White coefficients are estimated on other paths.
        HedgingConfig training = hestonWorld(HedgeKind::BlackScholesImplied, rebalances, paths);
        training.seed = 99;
        HedgingConfig fitted = hestonWorld(HedgeKind::MinimumVariance, rebalances, paths);
        fitted.coefficients = fit_minimum_variance_delta(training);
        const HedgingResult minimumVariance = run_hedging_backtest(fitted);

        // Every run uses the same paths: the comparison is paired.
        const Real best = truth.standard_deviation.value;
        EXPECT_LT(best, 0.97 * implied.standard_deviation.value);
        EXPECT_LT(best, 1.01 * skew.standard_deviation.value);
        EXPECT_LT(best, 1.01 * minimumVariance.standard_deviation.value);
        // With a negative correlation between spot and volatility, the
        // Black-Scholes delta of a call is too high (section 11.4): both
        // corrections lower it, and both hedge better.
        EXPECT_LT(skew.standard_deviation.value, 0.97 * implied.standard_deviation.value);
        EXPECT_LT(minimumVariance.standard_deviation.value, 0.97 * implied.standard_deviation.value);
        EXPECT_LT(fitted.coefficients.a + fitted.coefficients.b * 0.5 + fitted.coefficients.c * 0.25, 0.0);

        // Bought at the model price: no hedge earns anything on average.
        for (const HedgingResult *r : {&implied, &skew, &truth, &minimumVariance})
        {
            EXPECT_NEAR(r->mean.value, 0.0, 4.0 * r->mean.standard_error);
            // Near expiry and away from the strike there is no time value
            // left to read a volatility from: a small share of the dates.
            EXPECT_GT(r->uncalibrated_steps, 0u);
            EXPECT_LT(r->uncalibrated_steps, rebalances * paths / 10);
        }
    }

    // Stochastic volatility leaves a risk that no delta removes: even the
    // true model stays above the noise floor of discrete hedging.
    TEST(Hedging, StochasticVolatilityLeavesMoreThanDiscretisationNoise)
    {
        const HedgingResult truth = run_hedging_backtest(hestonWorld(HedgeKind::TrueModel, 42, 600));
        EXPECT_GT(truth.standard_deviation.value, 1.2 * truth.discretisation_noise);
    }

    // Section 11.7. With a Black-Scholes delta hedge and a zero rate, the
    // hedged P&L is the change of the option's value minus the delta term:
    // gamma, theta, vega and the unexplained part add up to it.
    TEST(Hedging, PnlExplanationAddsUp)
    {
        HedgingConfig c = hestonWorld(HedgeKind::BlackScholesImplied, 42, 500);
        c.world.rate = 0.0;
        const HedgingResult r = run_hedging_backtest(c);
        const PnlExplanation &e = r.explanation;
        EXPECT_NEAR(e.gamma.value + e.theta.value + e.vega.value + e.unexplained.value, r.mean.value, 0.005 * r.premium);
        EXPECT_GT(e.gamma.value, 0.0);
        EXPECT_LT(e.theta.value, 0.0);
        // Volatility moves: vega carries part of the P&L from path to path.
        EXPECT_GT(e.vega.standard_error, 0.0);
        // What the greeks do not explain is small next to the moves of the
        // option they explain.
        EXPECT_LT(e.unexplained_step_deviation, 0.25 * e.option_step_deviation);
    }

    // In a Black-Scholes world gamma earns what theta costs.
    TEST(Hedging, GammaPaysForThetaInABlackScholesWorld)
    {
        HedgingConfig c = blackScholesWorld(0.2, 0.2, 252, 2000);
        c.world.rate = 0.0;
        const PnlExplanation e = run_hedging_backtest(c).explanation;
        EXPECT_GT(e.gamma.value, 0.0);
        EXPECT_NEAR(e.gamma.value + e.theta.value, 0.0, 0.05 * e.gamma.value);
    }

    // In a Black-Scholes world the volatility does not move: no vega P&L, and
    // the unexplained part shrinks as the hedge gets more frequent.
    TEST(Hedging, UnexplainedShrinksWithTheRebalancingInterval)
    {
        const HedgingResult coarse = run_hedging_backtest(blackScholesWorld(0.2, 0.2, 16, 1000));
        const HedgingResult fine = run_hedging_backtest(blackScholesWorld(0.2, 0.2, 256, 1000));
        EXPECT_EQ(coarse.explanation.vega.value, 0.0);
        const Real coarseShare = coarse.explanation.unexplained_step_deviation / coarse.explanation.option_step_deviation;
        const Real fineShare = fine.explanation.unexplained_step_deviation / fine.explanation.option_step_deviation;
        EXPECT_LT(fineShare, 0.5 * coarseShare);
    }

    // Costs: hedging more often cuts the noise and raises the bill (Leland
    // 1985). The mean P&L falls with the cost rate, and falls further when
    // the hedge is more frequent.
    TEST(Hedging, TransactionCostsGrowWithTheHedgingFrequency)
    {
        const auto mean = [](Index rebalances, Real cost)
        {
            HedgingConfig c = blackScholesWorld(0.2, 0.2, rebalances, 2000);
            c.cost_rate = cost;
            return run_hedging_backtest(c).mean.value;
        };
        const Real freeCoarse = mean(16, 0.0);
        const Real paidCoarse = mean(16, 0.002);
        const Real freeFine = mean(128, 0.0);
        const Real paidFine = mean(128, 0.002);
        EXPECT_LT(paidCoarse, freeCoarse - 0.05);
        EXPECT_LT(paidFine - freeFine, 1.5 * (paidCoarse - freeCoarse));
    }

    // Determinism: same seed, same P&L on every path, whatever the threads.
    TEST(Hedging, DoesNotDependOnTheNumberOfThreads)
    {
        HedgingConfig c = hestonWorld(HedgeKind::TrueModel, 21, 100);
        c.threads = 1;
        const HedgingResult one = run_hedging_backtest(c);
        c.threads = 7;
        const HedgingResult seven = run_hedging_backtest(c);
        EXPECT_EQ(one.pnl, seven.pnl);
        EXPECT_EQ(one.standard_deviation.value, seven.standard_deviation.value);
        c.seed += 1;
        EXPECT_NE(run_hedging_backtest(c).pnl, one.pnl);

        HedgingConfig fit = hestonWorld(HedgeKind::BlackScholesImplied, 21, 100);
        fit.threads = 1;
        const MinimumVarianceCoefficients a = fit_minimum_variance_delta(fit);
        fit.threads = 5;
        const MinimumVarianceCoefficients b = fit_minimum_variance_delta(fit);
        EXPECT_EQ(a.a, b.a);
        EXPECT_EQ(a.b, b.b);
        EXPECT_EQ(a.c, b.c);
    }

    // A put is hedged like a call: by parity the two P&L differ only by the
    // forward, which the hedge replicates exactly.
    TEST(Hedging, PutAndCallLeaveTheSamePnl)
    {
        HedgingConfig call = blackScholesWorld(0.2, 0.2, 32, 300);
        HedgingConfig put = call;
        put.option.is_call = false;
        const HedgingResult a = run_hedging_backtest(call);
        const HedgingResult b = run_hedging_backtest(put);
        for (Index p = 0; p < a.pnl.size(); ++p)
        {
            ASSERT_NEAR(a.pnl[p], b.pnl[p], 1e-8);
        }
    }

    TEST(Hedging, EveryFigureHasItsUncertaintyAndTheResultSaysWhatItIs)
    {
        const HedgingResult r = run_hedging_backtest(hestonWorld(HedgeKind::BlackScholesImplied, 21, 300));
        for (const Estimate *e : {&r.mean, &r.standard_deviation, &r.explanation.delta, &r.explanation.unexplained})
        {
            EXPECT_GT(e->standard_error, 0.0);
            EXPECT_LT(e->lower, e->upper);
            EXPECT_FALSE(e->method.empty());
            EXPECT_FALSE(e->convention.empty());
        }
        EXPECT_FALSE(r.gamma_integral.has_value());
        EXPECT_LT(r.quantile_01, r.quantile_05);
        EXPECT_LT(r.quantile_95, r.quantile_99);
        EXPECT_NE(r.convention.find("Heston"), std::string::npos);
        EXPECT_NE(r.convention.find("quantModeling"), std::string::npos);
        EXPECT_GT(r.premium, 0.0);
    }

    TEST(Hedging, RefusesInconsistentConfigurationsAndCancels)
    {
        HedgingConfig c = blackScholesWorld(0.2, 0.2, 16, 100);
        c.hedge_volatility = 0.0;
        EXPECT_THROW(run_hedging_backtest(c), Error);
        c = blackScholesWorld(0.2, 0.2, 0, 100);
        EXPECT_THROW(run_hedging_backtest(c), Error);
        c = blackScholesWorld(0.2, 0.2, 16, 1);
        EXPECT_THROW(run_hedging_backtest(c), Error);
        c = hestonWorld(HedgeKind::TrueModel, 16, 100);
        c.world.heston.rho = 1.0;
        EXPECT_THROW(run_hedging_backtest(c), Error);
        c = blackScholesWorld(0.2, 0.2, 16, 100);
        c.option.strike = -1.0;
        EXPECT_THROW(run_hedging_backtest(c), Error);

        c = hestonWorld(HedgeKind::BlackScholesImplied, 21, 5000);
        std::atomic<bool> cancel{false};
        RunControl control;
        control.cancel = &cancel;
        control.progress = [&](double fraction)
        { cancel = fraction > 0.05; };
        EXPECT_THROW(run_hedging_backtest(c, control), Cancelled);
    }

} // namespace quantBacktest
