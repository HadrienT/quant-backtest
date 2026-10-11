import numpy as np
import pytest

import quantbacktest as qb

BLACK_SCHOLES = {
    "model": "black_scholes",
    "spot": 100.0,
    "rate": 0.02,
    "dividend_yield": 0.0,
    "volatility": 0.2,
}
HESTON = {
    "model": "heston",
    "spot": 100.0,
    "rate": 0.01,
    "dividend_yield": 0.0,
    "v0": 0.04,
    "kappa": 2.0,
    "theta": 0.04,
    "xi": 0.5,
    "rho": -0.7,
    "substeps": 4,
}
ESTIMATE_KEYS = {
    "value",
    "standard_error",
    "lower",
    "upper",
    "confidence",
    "method",
    "convention",
}


def test_the_wheel_was_built_against_a_pinned_pricing_library():
    assert len(qb.build_info()["quantmodeling_sha"]) == 40


def test_mean_pnl_is_the_gamma_weighted_variance_gap():
    result = qb.hedging_backtest(
        BLACK_SCHOLES,
        strike=100.0,
        maturity=0.25,
        hedge="black_scholes_fixed",
        hedge_volatility=0.15,
        rebalances=126,
        paths=2000,
        seed=1,
        threads=2,
    )
    assert set(result["mean"]) == ESTIMATE_KEYS
    assert result["pnl"].shape == (2000,)
    mean, integral = result["mean"], result["gamma_integral"]
    # Bought below the realised volatility: the buyer earns the gap.
    assert mean["value"] > 10 * mean["standard_error"]
    assert abs(mean["value"] - integral["value"]) < 0.05 * integral["value"]
    assert result["pnl"].mean() == pytest.approx(mean["value"])
    assert result["standard_deviation"]["value"] == pytest.approx(
        result["pnl"].std(ddof=1)
    )
    assert "Black-Scholes world" in result["convention"]


def test_corrected_deltas_hedge_better_than_black_scholes_under_heston():
    common = dict(strike=100.0, maturity=0.25, rebalances=21, paths=600, threads=2)
    coefficients = qb.fit_minimum_variance_delta(HESTON, seed=99, **common)
    assert set(coefficients) == {"a", "b", "c"}
    deviation = {}
    for hedge in ("black_scholes_implied", "minimum_variance", "true_model"):
        result = qb.hedging_backtest(
            HESTON, hedge=hedge, seed=7, coefficients=coefficients, **common
        )
        deviation[hedge] = result["standard_deviation"]["value"]
        assert result["gamma_integral"] is None
        assert result["explanation"]["vega"]["standard_error"] > 0
    assert deviation["true_model"] < deviation["black_scholes_implied"]
    assert deviation["minimum_variance"] < deviation["black_scholes_implied"]


def test_same_seed_same_paths_whatever_the_threads():
    kwargs = dict(
        strike=100.0, maturity=0.25, hedge="true_model", rebalances=10, paths=50, seed=3
    )
    one = qb.hedging_backtest(HESTON, threads=1, **kwargs)
    four = qb.hedging_backtest(HESTON, threads=4, **kwargs)
    assert (one["pnl"] == four["pnl"]).all()


def test_missing_parameters_are_refused():
    kwargs = dict(strike=100.0, maturity=0.25, rebalances=10, paths=50, seed=3)
    with pytest.raises(qb.Error, match="hedge_volatility"):
        qb.hedging_backtest(BLACK_SCHOLES, hedge="black_scholes_fixed", **kwargs)
    with pytest.raises(qb.Error, match="coefficients"):
        qb.hedging_backtest(HESTON, hedge="minimum_variance", **kwargs)
    with pytest.raises(qb.Error, match="unknown hedge"):
        qb.hedging_backtest(HESTON, hedge="delta", **kwargs)
    with pytest.raises(qb.Error, match="'rho'"):
        incomplete = {k: v for k, v in HESTON.items() if k != "rho"}
        qb.hedging_backtest(incomplete, hedge="true_model", **kwargs)
    with pytest.raises(qb.Error, match="unknown model"):
        qb.hedging_backtest({"model": "sabr"}, hedge="true_model", **kwargs)
    with pytest.raises(TypeError):
        qb.hedging_backtest(HESTON, hedge="true_model")
    assert np.isfinite(
        qb.hedging_backtest(HESTON, hedge="true_model", **kwargs)["pnl"]
    ).all()
