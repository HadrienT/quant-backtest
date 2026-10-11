#include <gtest/gtest.h>

#include "quantBacktest/core/error.hpp"
#include "quantBacktest/risk_models/pla_test.hpp"
#include "quantBacktest/risk_models/var_backtest.hpp"

#include <cmath>
#include <limits>
#include <random>
#include <vector>

namespace quantBacktest
{

    namespace
    {

        // Gaussian P&L of unit volatility against a constant VaR. `scale`
        // multiplies the exact VaR: 1 is an exact model, 0.5 a model that
        // announces half the loss it should.
        std::vector<std::uint8_t> simulate(Index days, Real quantile, Real scale, std::mt19937_64 &rng)
        {
            std::normal_distribution<Real> normal(0.0, 1.0);
            std::vector<Real> pnl(days);
            for (Real &x : pnl)
            {
                x = normal(rng);
            }
            const std::vector<Real> var(days, scale * quantile);
            return var_exceptions(pnl, var);
        }

        constexpr Real Quantile99 = 2.3263478740408408;
        constexpr Real Quantile95 = 1.6448536269514722;

        Real rejectionRate(Index days, Real tail, Real quantile, Real scale, int draws, std::uint64_t seed)
        {
            std::mt19937_64 rng(seed);
            int rejected = 0;
            for (int draw = 0; draw < draws; ++draw)
            {
                const CoverageTests tests = var_coverage_tests(simulate(days, quantile, scale, rng), tail);
                rejected += tests.unconditional_coverage.p_value < 0.05 ? 1 : 0;
            }
            return static_cast<Real>(rejected) / draws;
        }

    } // namespace

    TEST(VarBacktest, AnExceptionIsALossBeyondTheVar)
    {
        const std::vector<Real> pnl = {1.0, -2.0, -3.0, -3.1, 0.0};
        const std::vector<Real> var = {3.0, 3.0, 3.0, 3.0, 0.0};
        const std::vector<std::uint8_t> expected = {0, 0, 0, 1, 0};
        EXPECT_EQ(var_exceptions(pnl, var), expected);
        EXPECT_THROW(var_exceptions(pnl, std::vector<Real>{3.0}), Error);
        EXPECT_THROW(var_exceptions(pnl, std::vector<Real>{3.0, 3.0, -1.0, 3.0, 3.0}), Error);
        EXPECT_THROW(var_exceptions(std::vector<Real>{std::numeric_limits<Real>::quiet_NaN()}, std::vector<Real>{1.0}), Error);
    }

    TEST(VarBacktest, KupiecByHand)
    {
        // 250 days, 6 exceptions, p = 1 %.
        std::vector<std::uint8_t> exceptions(250, 0);
        for (const Index t : {10, 50, 90, 130, 170, 210})
        {
            exceptions[t] = 1;
        }
        const CoverageTests tests = var_coverage_tests(exceptions, 0.01);
        EXPECT_EQ(tests.observations, 250u);
        EXPECT_EQ(tests.exceptions, 6u);
        EXPECT_DOUBLE_EQ(tests.expected_exceptions, 2.5);
        const Real expected = -2.0 * (244.0 * std::log(0.99) + 6.0 * std::log(0.01) - 244.0 * std::log(244.0 / 250.0) - 6.0 * std::log(6.0 / 250.0));
        EXPECT_NEAR(tests.unconditional_coverage.statistic, expected, 1e-10);
        EXPECT_NEAR(tests.unconditional_coverage.statistic, 3.555, 0.001);
        EXPECT_NEAR(tests.unconditional_coverage.p_value, std::erfc(std::sqrt(0.5 * expected)), 1e-12);
        EXPECT_EQ(tests.unconditional_coverage.degrees_of_freedom, 1);

        // Exactly the expected rate: nothing to reject.
        std::vector<std::uint8_t> exact(200, 0);
        exact[20] = exact[120] = 1;
        EXPECT_NEAR(var_coverage_tests(exact, 0.01).unconditional_coverage.statistic, 0.0, 1e-12);
        EXPECT_NEAR(var_coverage_tests(exact, 0.01).unconditional_coverage.p_value, 1.0, 1e-6);

        // No exception at all: -2 T ln(1 - p).
        const std::vector<std::uint8_t> none(250, 0);
        EXPECT_NEAR(var_coverage_tests(none, 0.01).unconditional_coverage.statistic, -500.0 * std::log(0.99), 1e-10);
        EXPECT_THROW(var_coverage_tests(none, 0.0), Error);
        EXPECT_THROW(var_coverage_tests(std::vector<std::uint8_t>{1}, 0.01), Error);
    }

    // The chi-square tails used for the p-values, at their textbook critical
    // values.
    TEST(VarBacktest, PValuesAtTheUsualCriticalValues)
    {
        std::vector<std::uint8_t> exceptions(1000, 0);
        // Find a count whose Kupiec statistic is known by the formula, and
        // check the tail function through it.
        for (Index k = 0; k < 17; ++k)
        {
            exceptions[k * 50] = 1;
        }
        const CoverageTests tests = var_coverage_tests(exceptions, 0.01);
        const Real s = tests.unconditional_coverage.statistic;
        EXPECT_NEAR(tests.unconditional_coverage.p_value, std::erfc(std::sqrt(0.5 * s)), 1e-12);
        // chi-square(1) at 3.841 is 5 %; chi-square(2) at 5.991 is 5 %.
        EXPECT_NEAR(std::erfc(std::sqrt(0.5 * 3.841459)), 0.05, 1e-6);
        EXPECT_NEAR(std::exp(-0.5 * 5.991465), 0.05, 1e-6);
        EXPECT_NEAR(tests.conditional_coverage.p_value, std::exp(-0.5 * tests.conditional_coverage.statistic), 1e-12);
    }

    TEST(VarBacktest, ChristoffersenByHand)
    {
        // 0 0 1 1 0 0 0 1 0 0: n00 = 4, n01 = 2, n10 = 2, n11 = 1.
        const std::vector<std::uint8_t> exceptions = {0, 0, 1, 1, 0, 0, 0, 1, 0, 0};
        const CoverageTests tests = var_coverage_tests(exceptions, 0.3);
        EXPECT_EQ(tests.transitions[0][0], 4u);
        EXPECT_EQ(tests.transitions[0][1], 2u);
        EXPECT_EQ(tests.transitions[1][0], 2u);
        EXPECT_EQ(tests.transitions[1][1], 1u);
        const Real pi = 3.0 / 9.0;
        const Real pi01 = 2.0 / 6.0;
        const Real pi11 = 1.0 / 3.0;
        const Real expected = -2.0 * (6.0 * std::log(1.0 - pi) + 3.0 * std::log(pi) - 4.0 * std::log(1.0 - pi01) - 2.0 * std::log(pi01) - 2.0 * std::log(1.0 - pi11) - std::log(pi11));
        EXPECT_NEAR(tests.independence.statistic, expected, 1e-12);
        // The same probability of an exception after an exception as after a
        // quiet day: no dependence at all.
        EXPECT_NEAR(tests.independence.statistic, 0.0, 1e-12);
        EXPECT_NEAR(tests.conditional_coverage.statistic, tests.unconditional_coverage.statistic + tests.independence.statistic, 1e-12);
        EXPECT_EQ(tests.conditional_coverage.degrees_of_freedom, 2);
    }

    // The right number of exceptions is not enough: all in the same fortnight,
    // the count passes and the independence test does not.
    TEST(VarBacktest, ClusteredExceptionsPassKupiecAndFailIndependence)
    {
        std::vector<std::uint8_t> clustered(1000, 0);
        std::vector<std::uint8_t> spread(1000, 0);
        for (Index k = 0; k < 10; ++k)
        {
            clustered[500 + k] = 1;
            spread[50 + 100 * k] = 1;
        }
        const CoverageTests a = var_coverage_tests(clustered, 0.01);
        const CoverageTests b = var_coverage_tests(spread, 0.01);
        EXPECT_NEAR(a.unconditional_coverage.statistic, 0.0, 1e-12);
        EXPECT_NEAR(b.unconditional_coverage.statistic, 0.0, 1e-12);
        EXPECT_LT(a.independence.p_value, 1e-6);
        EXPECT_GT(b.independence.p_value, 0.5);
        EXPECT_LT(a.conditional_coverage.p_value, 1e-6);
        EXPECT_GT(b.conditional_coverage.p_value, 0.5);
    }

    // Without an exception there is nothing to test for independence: the
    // result is absent, not a comfortable zero.
    TEST(VarBacktest, IndependenceIsNotDefinedWithoutExceptions)
    {
        const CoverageTests none = var_coverage_tests(std::vector<std::uint8_t>(250, 0), 0.01);
        EXPECT_TRUE(std::isnan(none.independence.statistic));
        EXPECT_TRUE(std::isnan(none.independence.p_value));
        EXPECT_TRUE(std::isnan(none.conditional_coverage.p_value));
        EXPECT_FALSE(std::isnan(none.unconditional_coverage.p_value));
        const CoverageTests all = var_coverage_tests(std::vector<std::uint8_t>(50, 1), 0.01);
        EXPECT_TRUE(std::isnan(all.independence.statistic));
        EXPECT_LT(all.unconditional_coverage.p_value, 1e-12);
    }

    // Acceptance criterion of work package B7: on a simulated series whose
    // VaR is exact, the Kupiec test at 5 % rejects in 5 % +/- 2 % of the
    // draws.
    TEST(VarBacktest, KupiecHasItsNominalSizeOnAnExactModel)
    {
        EXPECT_NEAR(rejectionRate(1000, 0.05, Quantile95, 1.0, 4000, 1), 0.05, 0.02);
        EXPECT_NEAR(rejectionRate(2500, 0.01, Quantile99, 1.0, 4000, 2), 0.05, 0.02);
    }

    // The same test on the regulatory sample, 250 days at 99 %: 2.5
    // exceptions are expected, the count is an integer, and an exact model is
    // rejected about one time in ten, not one in twenty (no exception at all
    // is already a rejection). A fact to keep in sight, not a defect of the
    // code.
    TEST(VarBacktest, KupiecIsOversizedOnTwoHundredAndFiftyDays)
    {
        const Real rate = rejectionRate(250, 0.01, Quantile99, 1.0, 8000, 3);
        EXPECT_GT(rate, 0.08);
        EXPECT_LT(rate, 0.12);
        const std::vector<std::uint8_t> none(250, 0);
        EXPECT_LT(var_coverage_tests(none, 0.01).unconditional_coverage.p_value, 0.05);
    }

    // Acceptance criterion: a VaR understated by half is rejected almost
    // every time.
    TEST(VarBacktest, AVarUnderstatedByHalfIsRejected)
    {
        EXPECT_GT(rejectionRate(250, 0.01, Quantile99, 0.5, 2000, 4), 0.99);
        EXPECT_GT(rejectionRate(250, 0.05, Quantile95, 0.5, 2000, 5), 0.99);
    }

    // Independent exceptions: the independence test keeps its level, on a
    // sample large enough for consecutive exceptions to be common (4000 days
    // at 20 %). On the samples of practice it does not: measured at 1.7 % on
    // 250 days at 99 % and 8 % on 1000 days at 95 %, for 5 % announced.
    TEST(VarBacktest, IndependenceTestHasItsNominalSizeOnALargeSample)
    {
        std::mt19937_64 rng(6);
        const Real quantile80 = 0.8416212335729143;
        int rejected = 0;
        int conditional = 0;
        const int draws = 3000;
        for (int draw = 0; draw < draws; ++draw)
        {
            const CoverageTests tests = var_coverage_tests(simulate(4000, quantile80, 1.0, rng), 0.20);
            ASSERT_FALSE(std::isnan(tests.independence.p_value));
            rejected += tests.independence.p_value < 0.05 ? 1 : 0;
            conditional += tests.conditional_coverage.p_value < 0.05 ? 1 : 0;
        }
        EXPECT_NEAR(static_cast<Real>(rejected) / draws, 0.05, 0.02);
        EXPECT_NEAR(static_cast<Real>(conditional) / draws, 0.05, 0.02);
    }

    // The supervisory tables, as verified against the 1996 framework and the
    // Basel Framework (MAR33): 250 days at 99 %.
    TEST(TrafficLight, RegulatoryTable)
    {
        const std::vector<Real> plus = {0.0, 0.0, 0.0, 0.0, 0.0, 0.40, 0.50, 0.65, 0.75, 0.85, 1.00, 1.00, 1.00};
        const std::vector<Real> frtb = {1.50, 1.50, 1.50, 1.50, 1.50, 1.70, 1.76, 1.83, 1.88, 1.92, 2.00, 2.00, 2.00};
        for (Index x = 0; x < plus.size(); ++x)
        {
            const TrafficLight light = traffic_light(x, 250, 0.01);
            EXPECT_EQ(light.zone, x <= 4 ? Zone::Green : (x <= 9 ? Zone::Amber : Zone::Red)) << x << " exceptions";
            ASSERT_TRUE(light.basel_plus_factor.has_value());
            EXPECT_DOUBLE_EQ(*light.basel_plus_factor, plus[x]) << x << " exceptions";
            EXPECT_DOUBLE_EQ(*light.frtb_multiplier, frtb[x]) << x << " exceptions";
        }
        // Cumulative probabilities of the 1996 document.
        EXPECT_NEAR(traffic_light(4, 250, 0.01).cumulative_probability, 0.8922, 0.0001);
        EXPECT_NEAR(traffic_light(5, 250, 0.01).cumulative_probability, 0.9588, 0.0001);
        EXPECT_NEAR(traffic_light(9, 250, 0.01).cumulative_probability, 0.9997, 0.0001);
        EXPECT_GE(traffic_light(10, 250, 0.01).cumulative_probability, 0.9999);
        EXPECT_EQ(zone_name(Zone::Green), "green");
        EXPECT_EQ(zone_name(Zone::Amber), "amber");
        EXPECT_EQ(zone_name(Zone::Red), "red");
    }

    // Another sample has zones, by the same probabilities, but no supervisory
    // multiplier: none is invented.
    TEST(TrafficLight, OtherSamplesHaveZonesAndNoMultiplier)
    {
        const TrafficLight light = traffic_light(20, 500, 0.025);
        EXPECT_FALSE(light.basel_plus_factor.has_value());
        EXPECT_FALSE(light.frtb_multiplier.has_value());
        // 12.5 expected.
        EXPECT_EQ(traffic_light(12, 500, 0.025).zone, Zone::Green);
        EXPECT_EQ(traffic_light(20, 500, 0.025).zone, Zone::Amber);
        EXPECT_EQ(traffic_light(40, 500, 0.025).zone, Zone::Red);
        EXPECT_THROW(traffic_light(11, 10, 0.01), Error);
        EXPECT_THROW(traffic_light(1, 10, 1.0), Error);
    }

    // ---- P&L attribution test (MAR32.34 to MAR32.43) ----

    namespace
    {

        // A hypothetical P&L and a risk-theoretical one that misses part of
        // it: `missing` is the standard deviation of what the risk model does
        // not see, in units of the P&L's own.
        void desk(Index days, Real missing, std::mt19937_64 &rng, std::vector<Real> &hypothetical, std::vector<Real> &theoretical)
        {
            std::normal_distribution<Real> normal(0.0, 1.0);
            hypothetical.resize(days);
            theoretical.resize(days);
            for (Index t = 0; t < days; ++t)
            {
                theoretical[t] = normal(rng);
                hypothetical[t] = theoretical[t] + missing * normal(rng);
            }
        }

    } // namespace

    TEST(PlaTest, MetricsByHand)
    {
        // Ranks 1 2 3 4 5 against 2 1 4 3 5: Spearman 1 - 6 x 4 / 120 = 0.8.
        const std::vector<Real> a = {1.0, 2.0, 3.0, 4.0, 5.0};
        const std::vector<Real> b = {20.0, 10.0, 40.0, 30.0, 50.0};
        const PlaTest r = pla_test(a, b);
        EXPECT_NEAR(r.spearman, 0.8, 1e-12);
        // Every value of b is above every value of a: the two distribution
        // functions are as far apart as they can be.
        EXPECT_DOUBLE_EQ(r.kolmogorov_smirnov, 1.0);
        EXPECT_FALSE(r.zone.has_value());

        // The same series: perfect on both metrics.
        const PlaTest same = pla_test(a, a);
        EXPECT_DOUBLE_EQ(same.spearman, 1.0);
        EXPECT_DOUBLE_EQ(same.kolmogorov_smirnov, 0.0);
        EXPECT_DOUBLE_EQ(same.kolmogorov_smirnov_p_value, 1.0);

        // Ties share the mean of their ranks: 1, 2.5, 2.5, 4 on both sides.
        const std::vector<Real> tied = {1.0, 2.0, 2.0, 3.0};
        EXPECT_DOUBLE_EQ(pla_test(tied, tied).spearman, 1.0);
        // Shifted by one value out of four: the functions differ by 1/4 at most.
        const std::vector<Real> c = {1.0, 2.0, 3.0, 4.0};
        const std::vector<Real> d = {2.0, 3.0, 4.0, 5.0};
        EXPECT_DOUBLE_EQ(pla_test(c, d).kolmogorov_smirnov, 0.25);

        EXPECT_THROW(pla_test(a, c), Error);
        EXPECT_THROW(pla_test(std::vector<Real>{1.0, 2.0}, std::vector<Real>{1.0, 2.0}), Error);
        EXPECT_THROW(pla_test(std::vector<Real>(5, 1.0), a), Error);
    }

    // The p-values the regulatory text quotes next to its thresholds: 0.264
    // at 0.09 and 0.055 at 0.12, on 250 days.
    TEST(PlaTest, PValuesOfTheRegulatoryThresholds)
    {
        // Two samples of 250 whose distribution functions differ by a chosen
        // number of observations: b is a, shifted by half a step, with its
        // lowest values sent above everything.
        const auto apart = [](Index observations)
        {
            std::vector<Real> a(250);
            std::vector<Real> b(250);
            for (Index t = 0; t < 250; ++t)
            {
                a[t] = static_cast<Real>(t);
                b[t] = t + 1 < observations ? 1000.0 + static_cast<Real>(t) : static_cast<Real>(t) + 0.5;
            }
            return pla_test(a, b);
        };
        // 0.09 x 250 is not a whole number of observations: 23 of them, 0.092.
        const PlaTest atAmber = apart(23);
        EXPECT_NEAR(atAmber.kolmogorov_smirnov, 0.092, 1e-12);
        EXPECT_NEAR(atAmber.kolmogorov_smirnov_p_value, 0.264, 0.03);
        const PlaTest atRed = apart(30);
        EXPECT_NEAR(atRed.kolmogorov_smirnov, 0.12, 1e-12);
        EXPECT_NEAR(atRed.kolmogorov_smirnov_p_value, 0.055, 0.002);
    }

    TEST(PlaTest, ZonesOnTwoHundredAndFiftyDays)
    {
        std::mt19937_64 rng(20);
        std::vector<Real> hypothetical;
        std::vector<Real> theoretical;
        // A risk model that sees nearly everything: green. One that misses
        // half the variance: the correlation falls to about 0.7, amber or red.
        // One that sees nothing: red.
        int green = 0;
        int red = 0;
        int notGreen = 0;
        for (int draw = 0; draw < 200; ++draw)
        {
            desk(250, 0.2, rng, hypothetical, theoretical);
            green += *pla_test(hypothetical, theoretical).zone == Zone::Green ? 1 : 0;
            desk(250, 1.0, rng, hypothetical, theoretical);
            notGreen += *pla_test(hypothetical, theoretical).zone != Zone::Green ? 1 : 0;
            std::vector<Real> unrelated;
            std::vector<Real> other;
            desk(250, 0.0, rng, unrelated, other);
            desk(250, 0.0, rng, hypothetical, other);
            red += *pla_test(unrelated, hypothetical).zone == Zone::Red ? 1 : 0;
        }
        EXPECT_EQ(green, 200);
        EXPECT_EQ(notGreen, 200);
        EXPECT_EQ(red, 200);

        // Exactly on the thresholds: neither green nor red.
        desk(250, 0.2, rng, hypothetical, theoretical);
        const PlaTest r = pla_test(hypothetical, theoretical);
        EXPECT_GT(r.spearman, 0.9);
        EXPECT_LT(r.kolmogorov_smirnov, 0.09);
    }

    // The same variance but another shape: the correlation is perfect and the
    // distribution test fails. The two metrics do not measure the same thing.
    TEST(PlaTest, AScaleErrorPassesTheCorrelationAndFailsTheDistribution)
    {
        std::mt19937_64 rng(21);
        std::normal_distribution<Real> normal(0.0, 1.0);
        std::vector<Real> hypothetical(250);
        std::vector<Real> theoretical(250);
        for (Index t = 0; t < 250; ++t)
        {
            hypothetical[t] = normal(rng);
            theoretical[t] = 0.5 * hypothetical[t];
        }
        const PlaTest r = pla_test(hypothetical, theoretical);
        EXPECT_NEAR(r.spearman, 1.0, 1e-12);
        EXPECT_GT(r.kolmogorov_smirnov, 0.12);
        EXPECT_EQ(*r.zone, Zone::Red);
    }

    // MAR32.19: more than 12 exceptions at 99 %, or more than 30 at 97.5 %.
    TEST(DeskBacktest, ExceptionLimits)
    {
        const std::vector<Real> var99(250, 2.0);
        const std::vector<Real> var975(250, 1.0);
        const auto run = [&](Index beyond99, Index beyond975)
        {
            std::vector<Real> pnl(250, 0.0);
            for (Index t = 0; t < beyond99; ++t)
            {
                pnl[t] = -3.0;
            }
            for (Index t = beyond99; t < beyond99 + beyond975; ++t)
            {
                pnl[t] = -1.5;
            }
            return desk_backtest(pnl, var99, var975);
        };
        const DeskBacktest fine = run(12, 18);
        EXPECT_EQ(fine.exceptions_99, 12u);
        EXPECT_EQ(fine.exceptions_975, 30u);
        EXPECT_TRUE(fine.within_limits);
        EXPECT_FALSE(run(13, 0).within_limits);
        EXPECT_FALSE(run(5, 26).within_limits);
        EXPECT_THROW(desk_backtest(std::vector<Real>(10, 0.0), var99, var975), Error);
    }

} // namespace quantBacktest
