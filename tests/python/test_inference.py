import hashlib
import math

import numpy as np
import pytest

import quantbacktest as qb

ESTIMATE_KEYS = {
    "value",
    "standard_error",
    "lower",
    "upper",
    "confidence",
    "method",
    "convention",
}


def simulated(count=1000, seed=0):
    rng = np.random.default_rng(seed)
    returns = rng.normal(0.0006, 0.011, count)
    benchmark = rng.normal(0.0004, 0.010, count)
    cash = np.full(count, 0.0001)
    return returns, cash, benchmark


def test_performance_report_returns_every_figure_with_its_interval():
    returns, cash, benchmark = simulated()
    report = qb.performance_report(
        returns,
        cash,
        benchmark,
        bootstrap_replications=300,
        mean_block_length=5.0,
        seed=1,
        threads=2,
    )
    assert report["observations"] == 1000
    assert report["periods_per_year"] == 252.0
    assert report["omitted"] == {}
    assert len(report["metrics"]) == 14
    for name, estimate in report["metrics"].items():
        assert set(estimate) == ESTIMATE_KEYS, name
        assert estimate["lower"] < estimate["upper"], name
        assert estimate["method"] and estimate["convention"], name
    excess = returns - cash
    sharpe = excess.mean() / excess.std(ddof=1) * math.sqrt(252.0)
    assert report["metrics"]["sharpe_ratio"]["value"] == pytest.approx(sharpe)
    assert 0.0 < report["probabilistic_sharpe_ratio"] < 1.0


def test_same_seed_same_report_whatever_the_threads():
    returns, cash, _ = simulated()
    kwargs = dict(bootstrap_replications=200, mean_block_length=10.0, seed=5)
    one = qb.performance_report(returns, cash, **kwargs, threads=1)
    four = qb.performance_report(returns, cash, **kwargs, threads=4)
    assert one == four
    other = qb.performance_report(
        returns, cash, bootstrap_replications=200, mean_block_length=10.0, seed=6
    )
    assert other["metrics"]["max_drawdown"] != one["metrics"]["max_drawdown"]


def test_without_bootstrap_only_the_analytical_sharpe_ratio_is_returned():
    returns, cash, _ = simulated()
    report = qb.performance_report(
        returns, cash, bootstrap_replications=0, mean_block_length=1.0, seed=0
    )
    assert list(report["metrics"]) == ["sharpe_ratio"]
    assert "max_drawdown" in report["omitted"]


def test_seed_and_bootstrap_size_have_no_default():
    returns, cash, _ = simulated()
    with pytest.raises(TypeError):
        qb.performance_report(returns, cash)


def test_a_single_rate_is_not_a_rate_series():
    returns, cash, _ = simulated()
    with pytest.raises(ValueError, match="one rate per return"):
        qb.performance_report(
            returns,
            cash[:1],
            bootstrap_replications=0,
            mean_block_length=1.0,
            seed=0,
        )
    with pytest.raises(qb.Error):
        qb.performance_report(
            returns.reshape(10, 100),
            cash,
            bootstrap_replications=0,
            mean_block_length=1.0,
            seed=0,
        )


def test_progress_and_cancel():
    returns, cash, _ = simulated()
    seen = []
    qb.performance_report(
        returns,
        cash,
        bootstrap_replications=200,
        mean_block_length=1.0,
        seed=0,
        progress=seen.append,
    )
    assert seen == sorted(seen)
    assert seen[-1] == 1.0

    with pytest.raises(qb.Cancelled):
        qb.performance_report(
            returns,
            cash,
            bootstrap_replications=5000,
            mean_block_length=1.0,
            seed=0,
            threads=2,
            cancel=lambda: True,
        )
    assert issubclass(qb.Cancelled, qb.Error)
    assert issubclass(qb.Error, ValueError)


def test_factor_regression_annualises_alpha_with_its_standard_error():
    rng = np.random.default_rng(3)
    factors = rng.normal(0.0003, 0.01, (2520, 2))
    excess = 0.0004 + factors @ np.array([1.1, -0.3]) + rng.normal(0, 0.004, 2520)
    result = qb.factor_regression(excess, factors, newey_west_lags=5)
    alpha = result["alpha"]
    assert set(alpha) == ESTIMATE_KEYS
    assert abs(alpha["value"] - 0.0004 * 252) < 3 * alpha["standard_error"]
    assert alpha["standard_error"] > 0
    assert len(result["betas"]) == 2
    assert result["betas"][0]["value"] == pytest.approx(1.1, abs=0.05)
    assert result["betas"][1]["value"] == pytest.approx(-0.3, abs=0.05)
    assert result["newey_west_lags"] == 5
    assert result["observations"] == 2520
    # Against numpy's least squares.
    design = np.column_stack([np.ones(2520), factors])
    coefficients = np.linalg.lstsq(design, excess, rcond=None)[0]
    assert alpha["value"] == pytest.approx(coefficients[0] * 252)

    with pytest.raises(TypeError):
        qb.factor_regression(excess, factors)
    with pytest.raises(ValueError, match="two-dimensional"):
        qb.factor_regression(excess, factors[:, 0], newey_west_lags=0)


def test_fingerprint_is_the_sha256_of_type_shape_and_bytes():
    a = np.arange(6, dtype=np.float64)
    dates = np.array(["2024-01-02", "2024-01-03"], dtype="datetime64[D]")

    def expected(*arrays):
        h = hashlib.sha256()
        for x in arrays:
            name = str(x.dtype).encode()
            h.update(len(name).to_bytes(8, "little"))
            h.update(name)
            h.update(x.ndim.to_bytes(8, "little"))
            for n in x.shape:
                h.update(n.to_bytes(8, "little"))
            h.update(np.ascontiguousarray(x).tobytes())
        return h.hexdigest()

    assert qb.fingerprint(a) == expected(a)
    assert qb.fingerprint(a, dates) == expected(a, dates)
    # Same values, other shape, type or order: other data.
    assert qb.fingerprint(a.reshape(2, 3)) != qb.fingerprint(a)
    assert qb.fingerprint(a.astype(np.float32)) != qb.fingerprint(a)
    assert qb.fingerprint(dates, a) != qb.fingerprint(a, dates)
    # A view that is not contiguous hashes as its values.
    assert qb.fingerprint(a[::2]) == expected(a[::2].copy())
    with pytest.raises(ValueError):
        qb.fingerprint(np.array([object()]))


def test_default_rules_for_block_length_and_lags():
    rng = np.random.default_rng(8)
    noise = rng.normal(0.0, 1.0, 4000)
    persistent = np.empty(4000)
    persistent[0] = noise[0]
    for t in range(1, 4000):
        persistent[t] = 0.7 * persistent[t - 1] + noise[t]
    short = qb.optimal_block_length(noise)
    long = qb.optimal_block_length(persistent)
    assert 1.0 <= short < 5.0
    assert long > 5 * short
    assert qb.newey_west_lag(1000) == 6
    assert qb.newey_west_lag(100) == 4
    # They plug into the functions that have no default.
    report = qb.performance_report(
        0.01 * persistent,
        np.zeros(4000),
        bootstrap_replications=100,
        mean_block_length=long,
        seed=1,
    )
    assert "sharpe_ratio_bootstrap" in report["metrics"]
    with pytest.raises(qb.Error):
        qb.optimal_block_length(np.ones(100))
