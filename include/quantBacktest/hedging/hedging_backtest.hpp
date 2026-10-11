#ifndef QUANTBACKTEST_HEDGING_HEDGING_BACKTEST_HPP
#define QUANTBACKTEST_HEDGING_HEDGING_BACKTEST_HPP

#include "quantBacktest/core/estimate.hpp"
#include "quantBacktest/core/run_control.hpp"
#include "quantBacktest/core/types.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace quantBacktest
{

    // The hedging backtest of the companion, section 11, in a simulated world
    // (section 11.5; Hull and Suo, "A Methodology for Assessing Model Risk and
    // Its Application to the Implied Volatility Function Model", Journal of
    // Financial and Quantitative Analysis 37(2), 2002): the market follows a
    // known model, an option is bought and delta-hedged with another one, and
    // what is left in the P&L is measured. It needs no history of option
    // prices, and the truth is known.
    //
    // This is the only module that prices with the quantModeling library.
    // Nothing of that library appears here: the interface is plain data.

    /// Heston (1993) variance process: dv = kappa (theta - v) dt + xi sqrt(v) dW,
    /// correlated by rho with the Brownian motion of the spot.
    struct HestonParameters
    {
        Real v0 = 0.04;
        Real kappa = 1.5;
        Real theta = 0.04;
        Real xi = 0.3;
        Real rho = -0.6;
    };

    enum class WorldKind
    {
        BlackScholes,
        Heston,
    };

    /// The model the market really follows. Rates are continuous, per year.
    struct SimulatedWorld
    {
        WorldKind kind = WorldKind::BlackScholes;
        Real spot = 100.0;
        Real rate = 0.0;
        Real dividend_yield = 0.0;
        /// Realised volatility of the Black-Scholes world.
        Real volatility = 0.2;
        HestonParameters heston;
        /// Simulation steps inside each hedging interval, for Heston.
        Index substeps = 8;
    };

    /// A European vanilla option; the maturity is in years.
    struct HedgedOption
    {
        Real strike = 100.0;
        Real maturity = 1.0;
        bool is_call = true;
    };

    /// The delta the option is hedged with (section 11.4).
    enum class HedgeKind
    {
        /// Black-Scholes delta at one fixed volatility, `hedge_volatility`.
        /// The option is bought and marked at that same volatility: the
        /// experiment of the fundamental formula of section 11.1.
        BlackScholesFixed,
        /// Black-Scholes delta at the implied volatility of the day,
        /// recalibrated on the day's market price ("sticky strike").
        BlackScholesImplied,
        /// The same plus vega times the slope of the smile in strike: the
        /// local-volatility rule of thumb of Derman, "Regimes of Volatility",
        /// 1999. It is not a recalibrated Dupire model.
        SkewAdjusted,
        /// Hull and White, "Optimal Delta Hedging for Options", Journal of
        /// Banking and Finance 82, 2017: the Black-Scholes delta plus
        /// vega / (S sqrt(T)) (a + b delta + c delta^2), with coefficients
        /// estimated on data (`fit_minimum_variance_delta`).
        MinimumVariance,
        /// The minimum-variance delta of the model the world follows: the
        /// Black-Scholes delta at the true volatility, or under Heston
        /// dC/dS + rho xi / S dC/dv.
        TrueModel,
    };

    struct MinimumVarianceCoefficients
    {
        Real a = 0.0;
        Real b = 0.0;
        Real c = 0.0;
    };

    struct HedgingConfig
    {
        SimulatedWorld world;
        HedgedOption option;
        HedgeKind hedge = HedgeKind::BlackScholesImplied;
        /// For BlackScholesFixed only.
        Real hedge_volatility = 0.0;
        /// For MinimumVariance only.
        MinimumVarianceCoefficients coefficients;
        /// Number of rebalancings over the life of the option, equally
        /// spaced: 252 a year is daily.
        Index rebalances = 0;
        Index paths = 0;
        std::uint64_t seed = 0;
        /// The result does not depend on it.
        Index threads = 1;
        /// Proportional cost of each trade in the underlying.
        Real cost_rate = 0.0;
        Real confidence = 0.95;
    };

    /// The decomposition of section 11.7, with the Black-Scholes greeks of the
    /// previous rebalancing date at the volatility the option is marked at.
    /// Each term is summed over the life of the option; the estimates are
    /// their means over the paths. The five terms add up to the change of the
    /// option's value.
    struct PnlExplanation
    {
        Estimate delta;
        Estimate gamma;
        Estimate theta;
        Estimate vega;
        Estimate unexplained;
        /// Standard deviation, per rebalancing interval, of the unexplained
        /// part and of the change of the option's value it is a part of.
        Real unexplained_step_deviation = 0.0;
        Real option_step_deviation = 0.0;
    };

    /// Position: one option bought, hedged by selling delta shares, cash at
    /// the risk-free rate. The P&L is at maturity, in currency:
    /// payoff - hedge + cash, zero for a perfect hedge of a fairly priced
    /// option.
    struct HedgingResult
    {
        /// Price paid for the option.
        Real premium = 0.0;
        /// Mean and standard deviation of the P&L over the paths, with their
        /// Monte-Carlo standard errors. The standard deviation is the main
        /// criterion (section 11.3).
        Estimate mean;
        Estimate standard_deviation;
        Real skewness = 0.0;
        Real kurtosis = 0.0;
        /// Quantiles of the P&L at 1 %, 5 %, 95 % and 99 %.
        Real quantile_01 = 0.0;
        Real quantile_05 = 0.0;
        Real quantile_95 = 0.0;
        Real quantile_99 = 0.0;
        /// First-order autocorrelation of the hedging error of each interval
        /// (change of the hedged position in excess of the risk-free rate),
        /// pooled over paths.
        Real step_error_autocorrelation = 0.0;

        /// The noise floor of discrete hedging (Derman and Kamal, "When You
        /// Cannot Hedge Continuously", 1999): sqrt(pi / 4) vega sigma / sqrt(N)
        /// at inception. A difference between two hedges only means something
        /// next to it.
        Real discretisation_noise = 0.0;

        /// For BlackScholesFixed: the mean over the paths of the integral of
        /// section 11.1, sum of e^{r(T-t)} (1/2) Gamma S^2 (sigma_t^2 -
        /// sigma_hedge^2) dt, which the mean P&L must match.
        std::optional<Estimate> gamma_integral;

        PnlExplanation explanation;

        /// One value per path.
        std::vector<Real> pnl;
        /// Rebalancing dates, over all paths, on which no implied volatility
        /// could be read: the option was worth its intrinsic value, to the
        /// accuracy of the pricer. It is then hedged as the forward it has
        /// become (or not at all), and marked at the last volatility read.
        Index uncalibrated_steps = 0;

        std::string convention;
    };

    /// Throws `Error` on an inconsistent configuration, `Cancelled` if asked
    /// to stop.
    HedgingResult run_hedging_backtest(const HedgingConfig &config, const RunControl &control = {});

    /// Estimates the coefficients of the minimum-variance delta on simulated
    /// paths of the world, by least squares of the change of the option's
    /// price left by a Black-Scholes delta hedge on vega / (S sqrt(T)) dS
    /// times 1, delta and delta squared (Hull and White 2017, equation 5).
    /// Uses `world`, `option`, `rebalances`, `paths`, `seed` and `threads` of
    /// the configuration. Estimate on one seed and hedge on another.
    MinimumVarianceCoefficients fit_minimum_variance_delta(const HedgingConfig &config, const RunControl &control = {});

} // namespace quantBacktest

#endif
