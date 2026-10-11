#ifndef QUANTBACKTEST_INFERENCE_BOOTSTRAP_HPP
#define QUANTBACKTEST_INFERENCE_BOOTSTRAP_HPP

#include "quantBacktest/core/random.hpp"
#include "quantBacktest/core/run_control.hpp"
#include "quantBacktest/core/types.hpp"

#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace quantBacktest
{

    /// Every random draw has its seed as an explicit parameter
    /// (docs/contract.md, section 4).
    struct BootstrapConfig
    {
        Index replications = 0;
        /// Expected length of a block, in periods; 1 is the ordinary
        /// bootstrap of independent observations.
        Real mean_block_length = 1.0;
        std::uint64_t seed = 0;
        /// The result does not depend on it.
        Index threads = 1;
    };

    /// One resample of the stationary bootstrap of Politis and Romano, "The
    /// Stationary Bootstrap", Journal of the American Statistical Association
    /// 89(428), 1994: blocks of random, geometrically distributed length, to
    /// keep the dependence in time. Each index follows the previous one
    /// (wrapping around the end) with probability 1 - 1/mean_block_length,
    /// and is drawn afresh otherwise. Fills `indices` with values below
    /// `observations`.
    void stationary_bootstrap_indices(Index observations, Real mean_block_length, Random &random, std::span<Index> indices);

    /// Computes `outputs` statistics on one resample, given as the indices of
    /// the observations to read. It is called from several threads at once:
    /// it must not write anywhere but in `out`.
    using BootstrapStatistic = std::function<void(std::span<const Index> indices, std::span<Real> out)>;

    /// Runs the statistic on `config.replications` resamples of
    /// `observations` observations. Returns one row per replication and one
    /// column per statistic, row-major. Replication b draws from its own
    /// stream of the generator, so the same seed gives the same table
    /// whatever the number of threads.
    ///
    /// Progress is reported from the calling thread only.
    std::vector<Real> stationary_bootstrap(Index observations, Index outputs, const BootstrapStatistic &statistic,
                                           const BootstrapConfig &config, const RunControl &control = {});

    struct BootstrapInterval
    {
        Real lower = 0.0;
        Real upper = 0.0;
        /// Standard deviation of the replications.
        Real standard_error = 0.0;
    };

    /// Percentile interval at level `confidence` from the replications of one
    /// statistic: its (1 - confidence) / 2 and (1 + confidence) / 2 quantiles,
    /// by linear interpolation between order statistics. Every bound is NaN
    /// as soon as one replication is: a statistic that is not defined on some
    /// resamples has no interval.
    BootstrapInterval percentile_interval(std::span<const Real> replications, Real confidence);

} // namespace quantBacktest

#endif
