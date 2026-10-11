#include "quantBacktest/hedging/hedging_backtest.hpp"

#include "quantBacktest/core/parallel.hpp"
#include "quantBacktest/core/random.hpp"
#include "quantBacktest/metrics/performance.hpp"

#include "quantModeling/engines/analytic/heston_cos.hpp"
#include "quantModeling/models/equity/heston.hpp"
#include "quantModeling/models/equity/sabr.hpp"
#include "quantModeling/pricers/registry.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace quantBacktest
{

    namespace
    {

        namespace qm = quantModeling;

        constexpr Real kNaN = std::numeric_limits<Real>::quiet_NaN();
        // Largest relative strike bump for the derivatives in strike; it is
        // kept small next to the width of the distribution at maturity.
        constexpr Real kStrikeBump = 0.01;
        // Distance to the strike, in standard deviations of the log return to
        // maturity, beyond which an option is worth its intrinsic value.
        constexpr Real kFarFromStrike = 8.0;
        // Below this fraction of the spot, a time value is pricer noise.
        constexpr Real kMinimumTimeValue = 1e-6;

        // Price and Black-Scholes greeks of the option, by quantModeling.
        struct BlackScholesValue
        {
            Real price = 0.0;
            Real delta = 0.0;
            Real gamma = 0.0;
            Real vega = 0.0;
            Real theta = 0.0;
        };

        BlackScholesValue blackScholes(const SimulatedWorld &world, const HedgedOption &option, Real spot, Real ttm, Real volatility)
        {
            const qm::VanillaBSInput in{spot, option.strike, ttm, world.rate, world.dividend_yield, volatility, option.is_call};
            const qm::PricingRequest request{qm::InstrumentKind::EquityVanillaOption, qm::ModelKind::BlackScholes,
                                             qm::EngineKind::Analytic, qm::PricingInput{in}};
            const qm::PricingResult result = qm::default_registry().price(request);
            return {result.npv, result.greeks.delta.value_or(kNaN), result.greeks.gamma.value_or(kNaN),
                    result.greeks.vega.value_or(kNaN), result.greeks.theta.value_or(kNaN)};
        }

        qm::HestonParams hestonParams(const HestonParameters &p, Real variance)
        {
            qm::HestonParams out;
            out.v0 = variance;
            out.kappa = p.kappa;
            out.theta = p.theta;
            out.xi = p.xi;
            out.rho = p.rho;
            return out;
        }

        // What the market shows on one date: the price of the option, its
        // implied volatility, and, when the hedge needs them, the slope of the
        // smile and the minimum-variance delta of the true model.
        struct MarketState
        {
            Real price = kNaN;
            Real implied = kNaN;
            Real smileSlope = kNaN; // d(implied volatility) / d(strike)
            Real trueDelta = kNaN;
        };

        class Market
        {
          public:
            Market(const SimulatedWorld &world, const HedgedOption &option, HedgeKind hedge)
                : world_(world), option_(option), hedge_(hedge) {}

            // Implied volatility of a price of the option at the given strike;
            // NaN when the price is, to the accuracy of the pricer, the
            // intrinsic value: no volatility can be read from it.
            Real impliedVolatility(Real price, Real spot, Real strike, Real ttm) const
            {
                const Real discount = std::exp(-world_.rate * ttm);
                const Real forward = spot * std::exp((world_.rate - world_.dividend_yield) * ttm);
                // The solver takes a call: a put goes through parity.
                const Real call = option_.is_call ? price : price + discount * (forward - strike);
                const Real timeValue = call - discount * std::max(forward - strike, 0.0);
                if (!(timeValue > kMinimumTimeValue * spot))
                {
                    return kNaN;
                }
                return qm::black76_implied_vol(call, forward, strike, ttm, discount);
            }

            // Delta of an option worth its intrinsic value: the forward it has
            // become, or nothing.
            Real intrinsicDelta(Real spot, Real ttm) const
            {
                const Real forward = spot * std::exp((world_.rate - world_.dividend_yield) * ttm);
                const Real shares = std::exp(-world_.dividend_yield * ttm);
                if (option_.is_call)
                {
                    return forward > option_.strike ? shares : 0.0;
                }
                return forward < option_.strike ? -shares : 0.0;
            }

            MarketState observe(Real spot, Real variance, Real ttm) const
            {
                MarketState state;
                if (world_.kind == WorldKind::BlackScholes)
                {
                    const BlackScholesValue value = blackScholes(world_, option_, spot, ttm, world_.volatility);
                    state.price = value.price;
                    state.implied = world_.volatility;
                    state.smileSlope = 0.0;
                    state.trueDelta = value.delta;
                    return state;
                }
                const Real discount = std::exp(-world_.rate * ttm);
                const Real forward = spot * std::exp((world_.rate - world_.dividend_yield) * ttm);
                const Real k = option_.strike;
                // Standard deviation of the log return to maturity, from the
                // expected integrated variance of the model.
                const HestonParameters &h = world_.heston;
                const Real floored = std::max(variance, 0.0);
                const Real integrated = h.theta * ttm + (floored - h.theta) * -std::expm1(-h.kappa * ttm) / h.kappa;
                const Real width = std::sqrt(std::max(integrated, 1e-12));
                // Further than this from the strike the option is worth its
                // intrinsic value to machine precision, and the cosine series
                // of the pricer no longer covers the strike (quant-modeling
                // issue on heston_cos_price): the value is known, the pricer
                // is not called.
                if (std::abs(std::log(forward / k)) > kFarFromStrike * width)
                {
                    state.price = discount * std::max(option_.is_call ? forward - k : k - forward, 0.0);
                    state.trueDelta = intrinsicDelta(spot, ttm);
                    return state;
                }
                const Real bump = k * std::clamp(0.1 * width, 1e-4, kStrikeBump);
                variance = floored;
                const qm::HestonParams params = hestonParams(world_.heston, variance);
                const bool needsStrikes = hedge_ == HedgeKind::SkewAdjusted || hedge_ == HedgeKind::TrueModel;
                if (!needsStrikes)
                {
                    state.price = qm::heston_cos_price(forward, k, ttm, discount, params, option_.is_call);
                    state.implied = impliedVolatility(state.price, spot, k, ttm);
                    return state;
                }
                const std::vector<Real> prices = qm::heston_cos_prices(forward, {k - bump, k, k + bump}, ttm, discount, params, option_.is_call);
                state.price = prices[1];
                state.implied = impliedVolatility(state.price, spot, k, ttm);
                if (hedge_ == HedgeKind::SkewAdjusted)
                {
                    const Real below = impliedVolatility(prices[0], spot, k - bump, ttm);
                    const Real above = impliedVolatility(prices[2], spot, k + bump, ttm);
                    state.smileSlope = (above - below) / (2.0 * bump);
                }
                else
                {
                    // The price is homogeneous of degree one in (spot, strike):
                    // dC/dS = (C - K dC/dK) / S, without bumping the spot.
                    const Real dStrike = (prices[2] - prices[0]) / (2.0 * bump);
                    const Real dSpot = (state.price - k * dStrike) / spot;
                    const Real step = std::max(1e-4, 0.01 * variance);
                    const Real up = qm::heston_cos_price(forward, k, ttm, discount, hestonParams(world_.heston, variance + step), option_.is_call);
                    const Real down = qm::heston_cos_price(forward, k, ttm, discount, hestonParams(world_.heston, std::max(variance - step, 0.0)), option_.is_call);
                    const Real dVariance = (up - down) / (variance + step - std::max(variance - step, 0.0));
                    state.trueDelta = dSpot + world_.heston.rho * world_.heston.xi / spot * dVariance;
                }
                return state;
            }

          private:
            const SimulatedWorld &world_;
            const HedgedOption &option_;
            HedgeKind hedge_;
        };

        // One step of the world over `dt`, in `substeps` pieces. Heston:
        // Euler scheme on the log price, the variance floored at zero inside
        // the drift and the diffusion ("full truncation", Lord, Koekkoek and
        // van Dijk 2010).
        void advance(const SimulatedWorld &world, Random &random, Real dt, Real &spot, Real &variance)
        {
            const Real drift = world.rate - world.dividend_yield;
            if (world.kind == WorldKind::BlackScholes)
            {
                const Real sigma = world.volatility;
                spot *= std::exp((drift - 0.5 * sigma * sigma) * dt + sigma * std::sqrt(dt) * random.normal());
                return;
            }
            const HestonParameters &h = world.heston;
            const Real step = dt / static_cast<Real>(world.substeps);
            const Real root = std::sqrt(step);
            const Real orthogonal = std::sqrt(1.0 - h.rho * h.rho);
            for (Index k = 0; k < world.substeps; ++k)
            {
                const Real z1 = random.normal();
                const Real z2 = h.rho * z1 + orthogonal * random.normal();
                const Real positive = std::max(variance, 0.0);
                spot *= std::exp((drift - 0.5 * positive) * step + std::sqrt(positive) * root * z1);
                variance += h.kappa * (h.theta - positive) * step + h.xi * std::sqrt(positive) * root * z2;
            }
        }

        void validate(const HedgingConfig &config)
        {
            const SimulatedWorld &w = config.world;
            const auto positive = [](Real x)
            { return std::isfinite(x) && x > 0.0; };
            if (!positive(w.spot) || !std::isfinite(w.rate) || !std::isfinite(w.dividend_yield))
            {
                throw Error("the simulated world needs a positive spot and finite rates");
            }
            if (w.kind == WorldKind::BlackScholes && !positive(w.volatility))
            {
                throw Error("the Black-Scholes world needs a positive volatility");
            }
            if (w.kind == WorldKind::Heston)
            {
                const HestonParameters &h = w.heston;
                if (!(std::isfinite(h.v0) && h.v0 >= 0.0) || !positive(h.kappa) || !positive(h.theta) || !positive(h.xi) || !(h.rho > -1.0 && h.rho < 1.0))
                {
                    throw Error("the Heston world needs v0 >= 0, kappa > 0, theta > 0, xi > 0 and a correlation strictly between -1 and 1");
                }
                if (w.substeps == 0)
                {
                    throw Error("the number of simulation steps per hedging interval must be at least 1");
                }
            }
            if (!positive(config.option.strike) || !positive(config.option.maturity))
            {
                throw Error("the option needs a positive strike and a positive maturity");
            }
            if (config.rebalances == 0 || config.paths < 2)
            {
                throw Error("a hedging backtest needs at least 1 rebalancing and 2 paths");
            }
            if (config.hedge == HedgeKind::BlackScholesFixed && !positive(config.hedge_volatility))
            {
                throw Error("hedging at a fixed volatility needs that volatility, positive");
            }
            if (!(std::isfinite(config.cost_rate) && config.cost_rate >= 0.0))
            {
                throw Error("the cost rate must be a non-negative number");
            }
            if (!(config.confidence > 0.0 && config.confidence < 1.0))
            {
                throw Error("the confidence level must be strictly between 0 and 1");
            }
        }

        // What one path leaves.
        struct PathResult
        {
            Real pnl = 0.0;
            Real gammaIntegral = 0.0;
            Real delta = 0.0;
            Real gamma = 0.0;
            Real theta = 0.0;
            Real vega = 0.0;
            Real unexplained = 0.0;
            // Sums over the rebalancing intervals, for the pooled statistics.
            Real errorProducts = 0.0;
            Real errorSquares = 0.0;
            Real unexplainedSquares = 0.0;
            Real optionSquares = 0.0;
            Index uncalibrated = 0;
        };

        PathResult simulatePath(const HedgingConfig &config, Index path)
        {
            const SimulatedWorld &world = config.world;
            const HedgedOption &option = config.option;
            const Market market(world, option, config.hedge);
            const bool fixed = config.hedge == HedgeKind::BlackScholesFixed;
            Random random(config.seed, path);

            const Index steps = config.rebalances;
            const Real dt = option.maturity / static_cast<Real>(steps);
            const Real growth = std::exp(world.rate * dt);
            const Real carry = std::expm1(world.dividend_yield * dt);

            Real spot = world.spot;
            Real variance = world.kind == WorldKind::Heston ? world.heston.v0 : world.volatility * world.volatility;

            // The value the option is marked at, its Black-Scholes greeks at
            // the marking volatility, and the delta sold against it.
            Real markVolatility = kNaN;
            BlackScholesValue greeks;
            Real mark = kNaN;
            Real hedge = 0.0;

            PathResult out;
            // Reads the market on one date: marks the option, and sets the
            // delta to hold until the next date.
            const auto calibrate = [&](Real ttm, bool first)
            {
                MarketState state;
                Real volatility = config.hedge_volatility;
                if (!fixed)
                {
                    state = market.observe(spot, variance, ttm);
                    if (!std::isfinite(state.price))
                    {
                        throw Error("the pricing library returned no price for the option");
                    }
                    volatility = state.implied;
                }
                const bool calibrated = volatility > 0.0;
                if (!calibrated)
                {
                    if (first)
                    {
                        throw Error("the option has no implied volatility at inception: its price is its intrinsic value");
                    }
                    // Marked at the last volatility that could be read.
                    ++out.uncalibrated;
                    volatility = markVolatility;
                }
                const BlackScholesValue value = blackScholes(world, option, spot, ttm, volatility);
                Real delta = calibrated ? value.delta : market.intrinsicDelta(spot, ttm);
                if (calibrated)
                {
                    switch (config.hedge)
                    {
                        case HedgeKind::SkewAdjusted:
                            delta += std::isfinite(state.smileSlope) ? value.vega * state.smileSlope : 0.0;
                            break;
                        case HedgeKind::MinimumVariance:
                            delta += value.vega / (spot * std::sqrt(ttm)) *
                                     (config.coefficients.a + config.coefficients.b * value.delta + config.coefficients.c * value.delta * value.delta);
                            break;
                        default:
                            break;
                    }
                }
                // The model's own delta needs no implied volatility.
                if (config.hedge == HedgeKind::TrueModel && std::isfinite(state.trueDelta))
                {
                    delta = state.trueDelta;
                }
                markVolatility = volatility;
                greeks = value;
                mark = fixed ? value.price : state.price;
                hedge = delta;
            };

            calibrate(option.maturity, true);
            const Real premium = mark;
            Real cash = -premium + hedge * spot - config.cost_rate * std::abs(hedge) * spot;
            // Option - hedge + cash: zero at inception, but for the cost of the first trade.
            Real value = mark - hedge * spot + cash;
            Real previousError = 0.0;

            for (Index k = 1; k <= steps; ++k)
            {
                const Real ttmBefore = option.maturity - static_cast<Real>(k - 1) * dt;
                const Real spotBefore = spot;
                const Real varianceBefore = variance;
                const Real markBefore = mark;
                const Real volatilityBefore = markVolatility;
                const BlackScholesValue greeksBefore = greeks;
                const Real hedgeBefore = hedge;

                // Section 11.1, for a hedge at a fixed volatility: realised
                // variance against the hedging one, weighted by dollar gamma.
                if (fixed)
                {
                    const Real realised = world.kind == WorldKind::Heston ? std::max(varianceBefore, 0.0) : world.volatility * world.volatility;
                    out.gammaIntegral += std::exp(world.rate * (ttmBefore - dt)) * 0.5 * greeksBefore.gamma * spotBefore * spotBefore *
                                         (realised - config.hedge_volatility * config.hedge_volatility) * dt;
                }

                advance(world, random, dt, spot, variance);
                // Financing of the cash, and the dividends owed on the shares sold.
                cash = cash * growth - hedgeBefore * spot * carry;

                const Real ttm = option.maturity - static_cast<Real>(k) * dt;
                if (k < steps)
                {
                    calibrate(ttm, false);
                    const Real traded = hedge - hedgeBefore;
                    cash += traded * spot - config.cost_rate * std::abs(traded) * spot;
                }
                else
                {
                    mark = std::max(option.is_call ? spot - option.strike : option.strike - spot, 0.0);
                    cash -= config.cost_rate * std::abs(hedgeBefore) * spot;
                    hedge = 0.0;
                    cash -= hedgeBefore * spot; // the shares sold are bought back
                }

                // Section 11.7: the change of the option's value, explained by
                // the greeks of the previous date.
                const Real change = mark - markBefore;
                const Real move = spot - spotBefore;
                const Real deltaTerm = greeksBefore.delta * move;
                const Real gammaTerm = 0.5 * greeksBefore.gamma * move * move;
                const Real thetaTerm = greeksBefore.theta * dt;
                const Real vegaTerm = k < steps ? greeksBefore.vega * (markVolatility - volatilityBefore) : 0.0;
                const Real unexplained = change - deltaTerm - gammaTerm - thetaTerm - vegaTerm;
                out.delta += deltaTerm;
                out.gamma += gammaTerm;
                out.theta += thetaTerm;
                out.vega += vegaTerm;
                out.unexplained += unexplained;
                out.unexplainedSquares += unexplained * unexplained;
                out.optionSquares += change * change;

                // Hedging error of the interval: growth of the hedged position
                // beyond the risk-free rate.
                const Real newValue = mark - hedge * spot + cash;
                const Real error = newValue - value * growth;
                out.errorSquares += error * error;
                out.errorProducts += k > 1 ? error * previousError : 0.0;
                previousError = error;
                value = newValue;
            }
            out.pnl = value;
            return out;
        }

        Estimate meanEstimate(const std::vector<Real> &values, Real confidence, const std::string &convention)
        {
            const Real n = static_cast<Real>(values.size());
            const Real z = normal_quantile(0.5 + 0.5 * confidence);
            Estimate out;
            out.value = mean(values);
            out.standard_error = standard_deviation(values) / std::sqrt(n);
            out.lower = out.value - z * out.standard_error;
            out.upper = out.value + z * out.standard_error;
            out.confidence = confidence;
            out.method = "mean over the simulated paths, Monte-Carlo standard error, normal interval";
            out.convention = convention;
            return out;
        }

        Real quantile(std::vector<Real> &sorted, Real p)
        {
            const Real position = p * static_cast<Real>(sorted.size() - 1);
            const Index below = static_cast<Index>(std::floor(position));
            const Index above = std::min(below + 1, sorted.size() - 1);
            return sorted[below] + (position - static_cast<Real>(below)) * (sorted[above] - sorted[below]);
        }

        const char *hedgeName(HedgeKind hedge)
        {
            switch (hedge)
            {
                case HedgeKind::BlackScholesFixed:
                    return "Black-Scholes delta at a fixed volatility, the option bought and marked at that volatility";
                case HedgeKind::BlackScholesImplied:
                    return "Black-Scholes delta at the implied volatility of each date";
                case HedgeKind::SkewAdjusted:
                    return "Black-Scholes delta plus vega times the slope of the smile in strike (local-volatility rule of thumb)";
                case HedgeKind::MinimumVariance:
                    return "minimum-variance delta of Hull and White (2017) with the given coefficients";
                case HedgeKind::TrueModel:
                    return "minimum-variance delta of the model the world follows";
            }
            return "";
        }

        // Runs fn, turning an error of the pricing library into ours.
        template <typename Function>
        auto guarded(Function &&fn)
        {
            try
            {
                return fn();
            }
            catch (const Error &)
            {
                throw;
            }
            catch (const qm::PricingError &error)
            {
                throw Error(std::string("the pricing library refused the inputs: ") + error.what());
            }
        }

    } // namespace

    HedgingResult run_hedging_backtest(const HedgingConfig &config, const RunControl &control)
    {
        validate(config);
        std::vector<PathResult> paths(config.paths);
        guarded([&]
                {
                    parallel_for(config.paths, config.threads, control, [&](Index path)
                                 { paths[path] = simulatePath(config, path); });
                    return 0; });

        HedgingResult out;
        out.pnl.resize(config.paths);
        std::vector<Real> integral(config.paths);
        std::vector<Real> delta(config.paths);
        std::vector<Real> gamma(config.paths);
        std::vector<Real> theta(config.paths);
        std::vector<Real> vega(config.paths);
        std::vector<Real> unexplained(config.paths);
        Real errorProducts = 0.0;
        Real errorSquares = 0.0;
        Real unexplainedSquares = 0.0;
        Real optionSquares = 0.0;
        for (Index p = 0; p < config.paths; ++p)
        {
            const PathResult &path = paths[p];
            out.pnl[p] = path.pnl;
            integral[p] = path.gammaIntegral;
            delta[p] = path.delta;
            gamma[p] = path.gamma;
            theta[p] = path.theta;
            vega[p] = path.vega;
            unexplained[p] = path.unexplained;
            errorProducts += path.errorProducts;
            errorSquares += path.errorSquares;
            unexplainedSquares += path.unexplainedSquares;
            optionSquares += path.optionSquares;
            out.uncalibrated_steps += path.uncalibrated;
        }

        // Inception: the premium and the noise floor.
        const SimulatedWorld &world = config.world;
        const Market market(world, config.option, config.hedge);
        const Real variance0 = world.kind == WorldKind::Heston ? world.heston.v0 : world.volatility * world.volatility;
        const Real volatility0 = config.hedge == HedgeKind::BlackScholesFixed
                                     ? config.hedge_volatility
                                     : guarded([&]
                                               { return market.observe(world.spot, variance0, config.option.maturity).implied; });
        const BlackScholesValue inception = guarded([&]
                                                    { return blackScholes(world, config.option, world.spot, config.option.maturity, volatility0); });
        out.premium = config.hedge == HedgeKind::BlackScholesFixed
                          ? inception.price
                          : guarded([&]
                                    { return market.observe(world.spot, variance0, config.option.maturity).price; });
        out.discretisation_noise = std::sqrt(std::numbers::pi / 4.0) * inception.vega * volatility0 / std::sqrt(static_cast<Real>(config.rebalances));

        const std::string unit = "in currency, at maturity, for one option bought and delta-hedged";
        out.mean = meanEstimate(out.pnl, config.confidence, "mean of the P&L " + unit);

        const Real n = static_cast<Real>(config.paths);
        const Real deviation = standard_deviation(out.pnl);
        out.skewness = skewness(out.pnl);
        out.kurtosis = kurtosis(out.pnl);
        const Real z = normal_quantile(0.5 + 0.5 * config.confidence);
        out.standard_deviation.value = deviation;
        // Asymptotic standard error of a standard deviation: s sqrt((kurtosis - 1) / (4 n)).
        out.standard_deviation.standard_error = std::isfinite(out.kurtosis) ? deviation * std::sqrt(std::max(out.kurtosis - 1.0, 0.0) / (4.0 * n)) : 0.0;
        out.standard_deviation.lower = deviation - z * out.standard_deviation.standard_error;
        out.standard_deviation.upper = deviation + z * out.standard_deviation.standard_error;
        out.standard_deviation.confidence = config.confidence;
        out.standard_deviation.method = "standard deviation over the simulated paths, asymptotic standard error with the sample kurtosis, normal interval";
        out.standard_deviation.convention = "standard deviation of the P&L " + unit;

        std::vector<Real> sorted = out.pnl;
        std::sort(sorted.begin(), sorted.end());
        out.quantile_01 = quantile(sorted, 0.01);
        out.quantile_05 = quantile(sorted, 0.05);
        out.quantile_95 = quantile(sorted, 0.95);
        out.quantile_99 = quantile(sorted, 0.99);
        out.step_error_autocorrelation = errorSquares > 0.0 ? errorProducts / errorSquares : 0.0;

        if (config.hedge == HedgeKind::BlackScholesFixed)
        {
            out.gamma_integral = meanEstimate(integral, config.confidence,
                                              "mean of the sum of e^{r(T-t)} (1/2) gamma S^2 (realised variance - hedging variance) dt " + unit);
        }

        const std::string explained = ", summed over the life of the option, in currency";
        out.explanation.delta = meanEstimate(delta, config.confidence, "delta x change of the spot" + explained);
        out.explanation.gamma = meanEstimate(gamma, config.confidence, "(1/2) gamma x squared change of the spot" + explained);
        out.explanation.theta = meanEstimate(theta, config.confidence, "theta x time elapsed" + explained);
        out.explanation.vega = meanEstimate(vega, config.confidence, "vega x change of the marking volatility" + explained);
        out.explanation.unexplained = meanEstimate(unexplained, config.confidence, "change of the option's value not explained by the four terms" + explained);
        const Real intervals = n * static_cast<Real>(config.rebalances);
        out.explanation.unexplained_step_deviation = std::sqrt(unexplainedSquares / intervals);
        out.explanation.option_step_deviation = std::sqrt(optionSquares / intervals);

        out.convention = std::string("simulated ") + (world.kind == WorldKind::Heston ? "Heston" : "Black-Scholes") + " world; hedge: " + hedgeName(config.hedge) +
                         "; " + std::to_string(config.rebalances) + " equally spaced rebalancings; the option is priced by the quantModeling library";
        return out;
    }

    MinimumVarianceCoefficients fit_minimum_variance_delta(const HedgingConfig &config, const RunControl &control)
    {
        HedgingConfig observed = config;
        observed.hedge = HedgeKind::BlackScholesImplied;
        observed.cost_rate = 0.0;
        observed.confidence = 0.95;
        validate(observed);

        // Normal equations of the regression, one accumulator per path.
        struct Sums
        {
            Real xx[3][3] = {};
            Real xy[3] = {};
        };
        std::vector<Sums> sums(observed.paths);
        const SimulatedWorld &world = observed.world;
        const HedgedOption &option = observed.option;
        guarded([&]
                {
                    parallel_for(observed.paths, observed.threads, control, [&](Index path)
                                 {
                        const Market market(world, option, HedgeKind::BlackScholesImplied);
                        Random random(observed.seed, path);
                        const Real dt = option.maturity / static_cast<Real>(observed.rebalances);
                        Real spot = world.spot;
                        Real variance = world.kind == WorldKind::Heston ? world.heston.v0 : world.volatility * world.volatility;
                        // The last interval ends on the payoff, where the smile
                        // has collapsed: it is left out.
                        for (Index k = 0; k + 1 < observed.rebalances; ++k)
                        {
                            const Real ttm = option.maturity - static_cast<Real>(k) * dt;
                            const MarketState before = market.observe(spot, variance, ttm);
                            const Real spotBefore = spot;
                            advance(world, random, dt, spot, variance);
                            const MarketState after = market.observe(spot, variance, ttm - dt);
                            if (!(before.implied > 0.0) || !std::isfinite(after.price))
                            {
                                continue;
                            }
                            const BlackScholesValue value = blackScholes(world, option, spotBefore, ttm, before.implied);
                            const Real move = spot - spotBefore;
                            const Real scale = value.vega / (spotBefore * std::sqrt(ttm)) * move;
                            const Real x[3] = {scale, scale * value.delta, scale * value.delta * value.delta};
                            const Real y = (after.price - before.price) - value.delta * move;
                            for (int i = 0; i < 3; ++i)
                            {
                                sums[path].xy[i] += x[i] * y;
                                for (int j = 0; j < 3; ++j)
                                {
                                    sums[path].xx[i][j] += x[i] * x[j];
                                }
                            }
                        } });
                    return 0; });

        Real a[3][4] = {};
        for (const Sums &s : sums)
        {
            for (int i = 0; i < 3; ++i)
            {
                a[i][3] += s.xy[i];
                for (int j = 0; j < 3; ++j)
                {
                    a[i][j] += s.xx[i][j];
                }
            }
        }
        // Gauss elimination with partial pivoting on the 3 x 3 system.
        for (int col = 0; col < 3; ++col)
        {
            int pivot = col;
            for (int row = col + 1; row < 3; ++row)
            {
                pivot = std::abs(a[row][col]) > std::abs(a[pivot][col]) ? row : pivot;
            }
            std::swap(a[col], a[pivot]);
            if (!(std::abs(a[col][col]) > 0.0))
            {
                throw Error("the minimum-variance delta cannot be estimated: the simulated deltas do not vary enough");
            }
            for (int row = col + 1; row < 3; ++row)
            {
                const Real factor = a[row][col] / a[col][col];
                for (int j = col; j < 4; ++j)
                {
                    a[row][j] -= factor * a[col][j];
                }
            }
        }
        Real solution[3] = {};
        for (int row = 2; row >= 0; --row)
        {
            Real value = a[row][3];
            for (int j = row + 1; j < 3; ++j)
            {
                value -= a[row][j] * solution[j];
            }
            solution[row] = value / a[row][row];
        }
        if (!std::isfinite(solution[0]) || !std::isfinite(solution[1]) || !std::isfinite(solution[2]))
        {
            throw Error("the minimum-variance delta cannot be estimated: the regression has no finite solution");
        }
        return {solution[0], solution[1], solution[2]};
    }

} // namespace quantBacktest
