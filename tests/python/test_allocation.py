import numpy as np
import pytest

import quantbacktest as qb


def market(days=320, assets=8, seed=0):
    rng = np.random.default_rng(seed)
    dates = np.arange("2020-01-01", "2023-01-01", dtype="datetime64[D]")
    dates = dates[np.is_busday(dates)][:days]
    volatility = rng.uniform(0.01, 0.03, assets)
    returns = rng.normal(0.0004, volatility, (days, assets))
    close = 100.0 * np.cumprod(1.0 + returns, axis=0)
    open_ = close * (1.0 + rng.normal(0.0, 0.002, (days, assets)))
    high = np.maximum(open_, close) * 1.004
    low = np.minimum(open_, close) * 0.996
    gap = np.diff(dates).astype("int64")
    cash = np.concatenate([[0.0], 0.03 * gap / 360.0])
    return dates, close, open_, high, low, cash


def run(**overrides):
    dates, close, open_, high, low, cash = market()
    kwargs = dict(
        method="minimum_variance",
        lookback=60,
        rebalance=20,
        initial_capital=1e6,
        proportional_cost=0.0005,
        open=open_,
        high=high,
        low=low,
        cash_rate=cash,
        max_weight=0.3,
    )
    kwargs.update(overrides)
    return (dates, close), qb.run_allocation(dates, close, **kwargs)


def test_strategy_comes_with_its_comparison_points():
    (dates, close), result = run()
    for name in ("strategy", "equal_weight", "buy_and_hold"):
        r = result[name]
        assert r["dates"].dtype == np.dtype("datetime64[D]")
        assert (r["dates"] == dates).all()
        assert r["value"].shape == (len(dates),)
        assert r["shares"].shape == close.shape
        assert r["value"][0] == 1e6
        # Conservation, from what the wheel returns.
        positions = (r["shares"] * close).sum(axis=1)
        assert np.allclose(r["value"], r["cash"] + positions, rtol=1e-9)
        change = np.diff(r["value"])
        explained = (r["position_pnl"] + r["dividends"] + r["interest"] - r["costs"])[
            1:
        ]
        assert np.allclose(change, explained, rtol=0, atol=1e-6)
    journal = result["strategy"]["journal"]
    assert set(journal["status"]) == {"filled"}
    assert (journal["execution_date"] > journal["decision_date"]).all()
    assert result["strategy"]["costs"].sum() > 0


def test_weights_are_within_the_bounds_decided_by_the_optimiser():
    _, result = run()
    weights = result["allocations"]["weights"]
    assert weights.shape[1] == 8
    assert np.allclose(weights.sum(axis=1), 1.0)
    assert weights.min() >= 0.0
    assert weights.max() <= 0.3
    assert (result["allocations"]["eligible"] == 8).all()
    assert np.isfinite(result["allocations"]["in_sample_sharpe"]).all()
    assert "minimum variance" in result["conventions"]["strategy"]
    assert "5 basis points" in result["conventions"]["costs"]
    assert "close of t+1" in result["conventions"]["execution"]


def test_an_infeasible_cap_is_an_error():
    with pytest.raises(qb.Error, match="infeasible"):
        run(max_weight=0.1)
    with pytest.raises(qb.Error, match="unknown method"):
        run(method="markowitz")
    with pytest.raises(qb.Error, match="explicit cost model") as _:
        run(timing="same_close", proportional_cost=0.0, spread_estimator=None)


def test_same_close_needs_a_cost_and_runs_with_one():
    _, result = run(timing="same_close")
    journal = result["strategy"]["journal"]
    assert (journal["execution_date"] == journal["decision_date"]).all()


def test_events_universe_and_estimated_spread():
    dates, close, open_, high, low, cash = market()
    universe = np.ones(close.shape, dtype=bool)
    universe[:, 0] = False
    dividends = (np.array([1, 2]), dates[[100, 150]], np.array([0.5, 0.25]))
    # A two-for-one split of asset 3 on day 200: raw prices halve.
    for t in (close, open_, high, low):
        t[200:, 3] /= 2.0
    splits = (np.array([3]), dates[[200]], np.array([2.0]))
    result = qb.run_allocation(
        dates,
        close,
        method="risk_parity",
        lookback=60,
        rebalance="monthly",
        initial_capital=1e6,
        proportional_cost=0.0002,
        open=open_,
        high=high,
        low=low,
        cash_rate=cash,
        universe=universe,
        dividends=dividends,
        splits=splits,
        spread_estimator="abdi_ranaldo",
        timing="next_open",
    )
    weights = result["allocations"]["weights"]
    assert (weights[:, 0] == 0).all()
    assert (weights[:, 1:] > 0).all()
    assert result["strategy"]["dividends"].sum() > 0
    # The split leaves no jump in the value.
    value = result["strategy"]["value"]
    assert abs(value[200] / value[199] - 1.0) < 0.05
    assert "Abdi and Ranaldo" in result["conventions"]["costs"]
    months = result["allocations"]["dates"].astype("datetime64[M]")
    assert len(set(months)) == len(months)


def test_bad_data_is_refused_with_a_readable_message():
    dates, close, *_ = market()
    bad = close.copy()
    bad[10, 2] = -1.0
    with pytest.raises(qb.Error, match="zero, negative or infinite"):
        qb.run_allocation(
            dates,
            bad,
            method="equal_weight",
            lookback=20,
            rebalance=5,
            initial_capital=1e6,
            proportional_cost=0.0,
        )
    with pytest.raises(qb.Error, match="datetime64"):
        qb.run_allocation(
            np.arange(len(dates)),
            close,
            method="equal_weight",
            lookback=20,
            rebalance=5,
            initial_capital=1e6,
            proportional_cost=0.0,
        )
    with pytest.raises(TypeError):
        qb.run_allocation(dates, close, method="equal_weight")


def test_sharpe_difference():
    rng = np.random.default_rng(1)
    common = rng.normal(0, 0.01, 1500)
    first = 0.002 + common
    second = 0.0005 + 0.9 * common + rng.normal(0, 0.004, 1500)
    d = qb.sharpe_difference(
        first, second, bootstrap_replications=300, mean_block_length=5.0, seed=2
    )
    assert d["analytical"]["value"] > 0
    assert d["p_value"] < 0.01
    assert d["bootstrap"]["lower"] > 0
    assert d["bootstrap_p_value"] < 0.05
    assert 0.8 < d["correlation"] < 1.0
    plain = qb.sharpe_difference(
        first, second, bootstrap_replications=0, mean_block_length=1.0, seed=0
    )
    assert plain["bootstrap"] is None and plain["bootstrap_p_value"] is None
    assert plain["analytical"] == d["analytical"]
