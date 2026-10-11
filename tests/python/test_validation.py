import numpy as np
import pytest

import quantbacktest as qb


def noise(rows=1000, columns=20, seed=0, skill=0.0):
    rng = np.random.default_rng(seed)
    returns = rng.normal(0.0, 0.01, (rows, columns))
    returns[:, 0] += skill / np.sqrt(252.0) * 0.01
    return returns


def test_walk_forward_chooses_on_the_past_only():
    returns = noise()
    result = qb.walk_forward(returns, train=250, test=50)
    assert result["first_row"] == 250
    assert result["returns"].shape == (750,)
    assert len(result["folds"]) == 15
    for fold in result["folds"]:
        assert fold["train_end"] == fold["test_begin"]
        window = returns[fold["train_begin"] : fold["train_end"]]
        sharpes = window.mean(axis=0) / window.std(axis=0, ddof=1)
        assert fold["chosen"] == sharpes.argmax()
    rows = np.arange(250, 1000)
    assert (result["returns"] == returns[rows, result["chosen"]]).all()

    # Another future, same choices before it.
    changed = returns.copy()
    changed[600:] *= -1.0
    other = qb.walk_forward(changed, train=250, test=50)
    assert (other["chosen"][:350] == result["chosen"][:350]).all()

    growing = qb.walk_forward(returns, train=250, test=50, expanding=True)
    assert all(fold["train_begin"] == 0 for fold in growing["folds"])


def test_cpcv_returns_a_distribution_of_out_of_sample_sharpes():
    returns = noise(rows=1200)
    result = qb.combinatorial_purged_cv(
        returns, groups=6, test_groups=2, purge=5, embargo=5
    )
    assert result["splits"] == 15
    assert result["paths"] == 5
    assert result["path_sharpes"].shape == (5,)
    assert result["path_returns"].shape == (5, 1200)
    path = result["path_returns"][0]
    expected = path.mean() / path.std(ddof=1) * np.sqrt(252.0)
    assert result["path_sharpes"][0] == pytest.approx(expected)
    assert result["chosen"].shape == (15,)


def test_overfitting_statistics_tell_noise_from_an_edge():
    plain = noise(columns=40, seed=3)
    deflated = qb.deflated_sharpe(plain)
    assert deflated["trials"] == 40
    assert deflated["sharpe"] > 0.5
    assert deflated["deflated_sharpe_ratio"] < 0.95
    overfit = qb.backtest_overfitting(plain, blocks=10)
    assert overfit["splits"] == 252
    assert 0.2 < overfit["probability"] < 0.8
    assert overfit["mean_in_sample_sharpe"] > overfit["mean_out_of_sample_sharpe"]

    skilled = noise(rows=2520, columns=40, seed=3, skill=2.5)
    deflated = qb.deflated_sharpe(skilled)
    assert deflated["best"] == 0
    assert deflated["deflated_sharpe_ratio"] > 0.95
    assert qb.backtest_overfitting(skilled, blocks=10)["probability"] < 0.05


def test_bad_inputs_are_refused():
    returns = noise()
    with pytest.raises(qb.Error, match="two-dimensional"):
        qb.walk_forward(returns[:, 0], train=100, test=10)
    with pytest.raises(qb.Error, match="even"):
        qb.backtest_overfitting(returns, blocks=7)
    with pytest.raises(TypeError):
        qb.walk_forward(returns)
    with pytest.raises(TypeError):
        qb.combinatorial_purged_cv(returns, groups=6, test_groups=2)
    returns[5, 5] = np.nan
    with pytest.raises(qb.Error, match="missing"):
        qb.deflated_sharpe(returns)


def test_run_allocation_with_a_growing_window():
    rng = np.random.default_rng(1)
    dates = np.arange("2021-01-01", "2022-06-01", dtype="datetime64[D]")
    dates = dates[np.is_busday(dates)][:300]
    volatility = np.array([0.005, 0.01, 0.015, 0.02, 0.03])
    close = 100.0 * np.cumprod(1.0 + rng.normal(0.0003, volatility, (300, 5)), axis=0)
    kwargs = dict(
        method="minimum_variance",
        lookback=60,
        rebalance=20,
        initial_capital=1e6,
        proportional_cost=0.0005,
    )
    sliding = qb.run_allocation(dates, close, **kwargs)
    growing = qb.run_allocation(dates, close, expanding=True, **kwargs)
    assert "every daily return" in growing["conventions"]["strategy"]
    assert not np.allclose(
        growing["allocations"]["weights"][-1], sliding["allocations"]["weights"][-1]
    )
