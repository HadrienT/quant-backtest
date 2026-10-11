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


def test_gross_run_and_cost_summary():
    _, result = run()
    assert result["gross"]["value"][-1] > result["strategy"]["value"][-1]
    assert result["gross"]["costs"].sum() == 0.0
    summary = result["cost_summary"]
    assert summary["annualized_turnover"] > 0
    # 5 basis points on twice the one-way turnover.
    expected = 0.0005 * 2 * summary["annualized_turnover"] * 1e4
    assert summary["annual_cost_bps"] == pytest.approx(expected, rel=0.02)
    assert set(summary["break_even_cost"]) >= {"value", "standard_error", "lower"}

    _, free = run(proportional_cost=0.0)
    assert free["gross"] is None
    assert free["cost_summary"]["annual_cost_bps"] == 0.0


def test_total_returns_and_period_rates():
    dates = np.array(
        ["2024-01-02", "2024-01-03", "2024-01-04", "2024-01-05", "2024-01-08"],
        dtype="datetime64[D]",
    )
    close = np.array(
        [[100.0, 50.0], [110.0, 50.0], [55.0, np.nan], [66.0, 52.0], [66.0, 52.0]]
    )
    dividends = (np.array([0, 0]), dates[[1, 2]], np.array([1.0, 0.5]))
    splits = (np.array([0]), dates[[2]], np.array([2.0]))
    returns = qb.total_returns(dates, close, dividends=dividends, splits=splits)
    assert returns.shape == close.shape
    assert np.isnan(returns[0]).all()
    assert returns[1, 0] == pytest.approx(111.0 / 100.0 - 1.0)
    assert returns[2, 0] == pytest.approx(2.0 * 55.5 / 110.0 - 1.0)
    # No return across a missing quote.
    assert np.isnan(returns[2, 1]) and np.isnan(returns[3, 1])
    assert returns[4, 1] == 0.0

    annual = np.array([0.036, 0.072, 0.072, 0.072, 0.5])
    rates = qb.period_rates(dates, annual, basis=360.0)
    assert rates[0] == 0.0
    assert rates[1] == pytest.approx(0.036 / 360.0)
    # Friday to Monday: three days, at the rate known on Friday.
    assert rates[4] == pytest.approx(0.072 * 3 / 360.0)
    with pytest.raises(TypeError):
        qb.period_rates(dates, annual)
    with pytest.raises(qb.Error):
        qb.period_rates(dates, annual[:2], basis=360.0)


def test_short_positions_whole_shares_and_impact():
    dates, close, open_, high, low, cash = market()
    rng = np.random.default_rng(5)
    volume = rng.uniform(1e5, 1e6, close.shape)
    # Asset 1 is a leveraged copy of asset 0: worth selling against it.
    returns0 = np.diff(close[:, 0]) / close[:-1, 0]
    close[1:, 1] = close[0, 1] * np.cumprod(1.0 + 1.5 * returns0)
    kwargs = dict(
        method="minimum_variance",
        lookback=60,
        rebalance=20,
        initial_capital=1e6,
        proportional_cost=0.0005,
        cash_rate=cash,
        volume=volume,
    )
    with pytest.raises(qb.Error, match="not allowed"):
        qb.run_allocation(dates, close, min_weight=-0.3, max_weight=0.6, **kwargs)
    with pytest.raises(qb.Error, match="borrow_rate"):
        qb.run_allocation(
            dates, close, min_weight=-0.3, max_weight=0.6, allow_short=True, **kwargs
        )
    borrow = np.full(len(dates), 0.02 / 252.0)
    result = qb.run_allocation(
        dates,
        close,
        min_weight=-0.3,
        max_weight=0.6,
        allow_short=True,
        borrow_rate=borrow,
        whole_shares=True,
        impact_coefficient=1.0,
        **kwargs,
    )
    assert result["allocations"]["weights"].min() < 0
    run = result["strategy"]
    assert run["shares"].min() < 0
    assert run["borrow_cost"].sum() > 0
    assert (run["journal"]["quantity"] == np.trunc(run["journal"]["quantity"])).all()
    change = np.diff(run["value"])
    explained = (
        run["position_pnl"]
        + run["dividends"]
        + run["interest"]
        - run["costs"]
        - run["borrow_cost"]
    )[1:]
    assert np.allclose(change, explained, rtol=0, atol=1e-6)
    assert "square-root market impact" in result["conventions"]["costs"]
    share = result["cost_summary"]["participation"]
    assert share["executions"] > 0
    assert 0 < share["median"] <= share["maximum"]

    with pytest.raises(qb.Error, match="volume"):
        qb.run_allocation(
            dates,
            close,
            method="equal_weight",
            lookback=60,
            rebalance=20,
            initial_capital=1e6,
            proportional_cost=0.0,
            impact_coefficient=1.0,
        )
    _, plain = run_default()
    assert plain["cost_summary"]["participation"] is None


def run_default():
    return run()


def test_grid_is_the_table_the_validation_takes():
    dates, close, open_, high, low, cash = market()
    grid = [
        {"method": method, "lookback": lookback, "rebalance": 20}
        for method in ("equal_weight", "minimum_variance", "risk_parity")
        for lookback in (40, 80)
    ]
    grid.append(
        {
            "method": "minimum_variance",
            "lookback": 60,
            "rebalance": "monthly",
            "max_weight": 0.3,
            "expanding": True,
            "covariance": "sample",
        }
    )
    common = dict(initial_capital=1e6, proportional_cost=0.0005, cash_rate=cash)
    table = qb.run_allocation_grid(dates, close, grid=grid, threads=3, **common)
    assert table["returns"].shape == (len(dates) - 1, 7)
    assert (table["dates"] == dates[1:]).all()
    assert len(table["descriptions"]) == 7
    assert "minimum variance" in table["descriptions"][2]

    # A column is the run of its configuration.
    single = qb.run_allocation(
        dates, close, method="minimum_variance", lookback=80, rebalance=20, **common
    )
    value = single["strategy"]["value"]
    assert np.array_equal(table["returns"][:, 3], value[1:] / value[:-1] - 1.0)
    assert table["final_values"][3] == value[-1]
    again = qb.run_allocation_grid(dates, close, grid=grid, threads=1, **common)
    assert np.array_equal(again["returns"], table["returns"])

    # And it feeds the validation as it is.
    assert qb.deflated_sharpe(table["returns"])["trials"] == 7
    assert qb.walk_forward(table["returns"], train=120, test=40)["first_row"] == 120

    with pytest.raises(qb.Error, match="configuration 1"):
        bad = [grid[0], dict(grid[2], max_weight=0.05)]
        qb.run_allocation_grid(dates, close, grid=bad, **common)
    with pytest.raises(qb.Error, match="unknown key"):
        qb.run_allocation_grid(dates, close, grid=[dict(grid[0], window=5)], **common)
    with pytest.raises(qb.Error, match="needs the key"):
        qb.run_allocation_grid(
            dates, close, grid=[{"method": "equal_weight"}], **common
        )
