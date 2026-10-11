#ifndef QUANTBACKTEST_RISK_MODELS_PLA_TEST_HPP
#define QUANTBACKTEST_RISK_MODELS_PLA_TEST_HPP

#include "quantBacktest/core/types.hpp"
#include "quantBacktest/risk_models/var_backtest.hpp"

#include <optional>
#include <span>

namespace quantBacktest
{

    /// The P&L attribution test of the Basel Framework, MAR32.34 to MAR32.43
    /// (companion, section 12.3): does the risk model see the same P&L as the
    /// desk? It compares, day by day, the hypothetical P&L (front-office
    /// systems, every risk factor) with the risk-theoretical P&L (the risk
    /// model, its own factors).
    struct PlaTest
    {
        Index observations = 0;
        /// Spearman correlation: correlation of the ranks of the two series
        /// (MAR32.36 to MAR32.38). Equal values share the mean of their ranks.
        Real spearman = 0.0;
        /// Kolmogorov-Smirnov metric: the largest absolute difference between
        /// the two empirical distribution functions (MAR32.39 to MAR32.41).
        Real kolmogorov_smirnov = 0.0;
        /// Probability of a metric at least as large when the two series come
        /// from the same distribution, by the asymptotic law with
        /// sqrt(n / 2): the one the text quotes next to its thresholds (0.264
        /// at 0.09 and 0.055 at 0.12, for 250 days).
        Real kolmogorov_smirnov_p_value = 0.0;
        /// The supervisory zone exists for one sample only, the most recent
        /// 250 trading days (MAR32.35); absent otherwise. Green: correlation
        /// above 0.80 and KS metric below 0.09. Red: correlation below 0.70
        /// or KS metric above 0.12, and the desk loses the internal model.
        /// Amber: neither.
        std::optional<Zone> zone;
    };

    /// The two series are aligned, one value per day.
    PlaTest pla_test(std::span<const Real> hypothetical, std::span<const Real> risk_theoretical);

    /// Desk-level backtesting, MAR32.19: a desk that shows more than 12
    /// exceptions at the 99th percentile or more than 30 at the 97.5th in the
    /// most recent 12 months must use the standardised approach.
    struct DeskBacktest
    {
        Index observations = 0;
        Index exceptions_99 = 0;
        Index exceptions_975 = 0;
        bool within_limits = true;
    };

    /// `pnl` is the P&L of each day of the last 12 months; the two VaR series
    /// are the ones announced for those days, as positive losses.
    DeskBacktest desk_backtest(std::span<const Real> pnl, std::span<const Real> value_at_risk_99, std::span<const Real> value_at_risk_975);

} // namespace quantBacktest

#endif
