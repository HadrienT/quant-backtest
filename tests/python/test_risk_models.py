import math

import numpy as np
import pytest

import quantbacktest as qb

QUANTILE_99 = 2.3263478740408408


def test_exact_model_on_the_regulatory_sample():
    pnl = np.zeros(250)
    var = np.full(250, 1.0)
    pnl[[20, 90, 200]] = -1.5
    result = qb.var_backtest(pnl, var, tail_probability=0.01)
    assert result["observations"] == 250
    assert result["exceptions"] == 3
    assert result["expected_exceptions"] == 2.5
    assert result["exception_days"].sum() == 3
    assert result["exception_days"][90] == 1
    assert result["unconditional_coverage"]["p_value"] > 0.5
    assert result["transitions"] == ((243, 3), (3, 0))
    light = result["traffic_light"]
    assert light["zone"] == "green"
    assert light["basel_plus_factor"] == 0.0
    assert light["frtb_multiplier"] == 1.5


def test_understated_var_is_rejected_and_red():
    rng = np.random.default_rng(0)
    pnl = rng.normal(0.0, 1.0, 250)
    var = np.full(250, 0.5 * QUANTILE_99)
    result = qb.var_backtest(pnl, var, tail_probability=0.01)
    assert result["exceptions"] > 15
    assert result["unconditional_coverage"]["p_value"] < 1e-6
    light = result["traffic_light"]
    assert light["zone"] == "red"
    assert light["basel_plus_factor"] == 1.0
    assert light["frtb_multiplier"] == 2.0


def test_no_supervisory_multiplier_outside_its_sample():
    pnl = np.zeros(500)
    var = np.full(500, 1.0)
    result = qb.var_backtest(pnl, var, tail_probability=0.025)
    assert result["traffic_light"]["basel_plus_factor"] is None
    assert result["traffic_light"]["frtb_multiplier"] is None
    # No exception: independence cannot be tested, and says so.
    assert math.isnan(result["independence"]["p_value"])
    assert not math.isnan(result["unconditional_coverage"]["p_value"])


def test_bad_inputs_are_refused():
    with pytest.raises(ValueError, match="same length"):
        qb.var_backtest(np.zeros(10), np.ones(9), tail_probability=0.01)
    with pytest.raises(ValueError, match="positive loss"):
        qb.var_backtest(np.zeros(10), -np.ones(10), tail_probability=0.01)
    with pytest.raises(TypeError):
        qb.var_backtest(np.zeros(10), np.ones(10))


def test_pla_test_and_desk_limits():
    rng = np.random.default_rng(4)
    theoretical = rng.normal(0.0, 1.0, 250)
    close = qb.pla_test(theoretical + 0.2 * rng.normal(0.0, 1.0, 250), theoretical)
    assert close["zone"] == "green"
    assert close["spearman"] > 0.9
    far = qb.pla_test(rng.normal(0.0, 1.0, 250), theoretical)
    assert far["zone"] == "red"
    scaled = qb.pla_test(2.0 * theoretical, theoretical)
    assert scaled["spearman"] == pytest.approx(1.0)
    assert scaled["zone"] == "red"
    assert qb.pla_test(theoretical[:100], theoretical[:100])["zone"] is None

    pnl = np.zeros(250)
    pnl[:13] = -3.0
    desk = qb.desk_backtest(pnl, np.full(250, 2.0), np.full(250, 1.0))
    assert desk["exceptions_99"] == 13
    assert desk["within_limits"] is False
