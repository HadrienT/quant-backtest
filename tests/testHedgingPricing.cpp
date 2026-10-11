// Lot B0: quantModeling, fetched at its pinned commit, links and prices. The
// hedging loop itself arrives with lot B6.
#include <gtest/gtest.h>

#include "quantBacktest/core/build_info.hpp"

#include "quantModeling/pricers/registry.hpp"

#include <cmath>

namespace quantBacktest
{

    namespace
    {

        constexpr double S0 = 100.0;
        constexpr double K = 105.0;
        constexpr double T = 0.75;
        constexpr double r = 0.03;
        constexpr double q = 0.01;
        constexpr double sigma = 0.25;

        double normalCdf(double x)
        {
            return 0.5 * std::erfc(-x / std::sqrt(2.0));
        }

        // Black and Scholes (1973), with a continuous dividend yield q.
        double blackScholesCall()
        {
            const double d1 = (std::log(S0 / K) + (r - q + 0.5 * sigma * sigma) * T) / (sigma * std::sqrt(T));
            const double d2 = d1 - sigma * std::sqrt(T);
            return S0 * std::exp(-q * T) * normalCdf(d1) - K * std::exp(-r * T) * normalCdf(d2);
        }

        double price(bool isCall)
        {
            namespace qm = quantModeling;
            const qm::VanillaBSInput in{S0, K, T, r, q, sigma, isCall};
            const qm::PricingRequest request{
                qm::InstrumentKind::EquityVanillaOption,
                qm::ModelKind::BlackScholes,
                qm::EngineKind::Analytic,
                qm::PricingInput{in}};
            return qm::default_registry().price(request).npv;
        }

    } // namespace

    TEST(HedgingPricing, CallMatchesClosedForm)
    {
        EXPECT_NEAR(price(true), blackScholesCall(), 1e-10);
    }

    TEST(HedgingPricing, PutCallParity)
    {
        const double forwardValue = S0 * std::exp(-q * T) - K * std::exp(-r * T);
        EXPECT_NEAR(price(true) - price(false), forwardValue, 1e-10);
    }

    TEST(HedgingPricing, BuildRecordsThePinnedCommit)
    {
        EXPECT_EQ(build_info().quantmodeling_sha.size(), 40u);
    }

} // namespace quantBacktest
