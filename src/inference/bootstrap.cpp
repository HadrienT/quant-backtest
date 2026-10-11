#include "quantBacktest/inference/bootstrap.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <exception>
#include <limits>
#include <mutex>
#include <thread>

namespace quantBacktest
{

    void stationary_bootstrap_indices(Index observations, Real mean_block_length, Random &random, std::span<Index> indices)
    {
        if (observations == 0)
        {
            throw Error("the bootstrap needs observations");
        }
        if (!(mean_block_length >= 1.0) || !std::isfinite(mean_block_length))
        {
            throw Error("the mean block length of the bootstrap must be at least 1");
        }
        const Real restart = 1.0 / mean_block_length;
        Index current = 0;
        for (Index k = 0; k < indices.size(); ++k)
        {
            if (k == 0 || random.uniform() < restart)
            {
                current = random.uniform_index(observations);
            }
            else
            {
                current = current + 1 == observations ? 0 : current + 1;
            }
            indices[k] = current;
        }
    }

    std::vector<Real> stationary_bootstrap(Index observations, Index outputs, const BootstrapStatistic &statistic,
                                           const BootstrapConfig &config, const RunControl &control)
    {
        if (config.replications == 0)
        {
            throw Error("the bootstrap needs at least one replication");
        }
        if (config.threads == 0)
        {
            throw Error("the number of threads must be at least 1");
        }
        // Validates the block length before any thread starts.
        {
            Random probe(config.seed);
            Index none = 0;
            stationary_bootstrap_indices(observations, config.mean_block_length, probe, {&none, 0});
        }

        const Index replications = config.replications;
        const Index workers = std::min(config.threads, replications);
        std::vector<Real> table(replications * outputs);
        std::atomic<Index> done{0};
        std::atomic<bool> stop{false};
        std::exception_ptr failure;
        std::mutex failureMutex;

        // Worker w takes replications w, w + workers, w + 2 workers, ...
        const auto work = [&](Index worker)
        {
            try
            {
                std::vector<Index> indices(observations);
                for (Index b = worker; b < replications && !stop.load(std::memory_order_relaxed); b += workers)
                {
                    control.check_cancelled();
                    Random random(config.seed, b);
                    stationary_bootstrap_indices(observations, config.mean_block_length, random, indices);
                    statistic(indices, {table.data() + b * outputs, outputs});
                    const Index finished = done.fetch_add(1, std::memory_order_relaxed) + 1;
                    if (worker == 0)
                    {
                        control.report(static_cast<double>(finished) / static_cast<double>(replications));
                    }
                }
            }
            catch (...)
            {
                stop.store(true, std::memory_order_relaxed);
                const std::lock_guard<std::mutex> lock(failureMutex);
                if (!failure)
                {
                    failure = std::current_exception();
                }
            }
        };

        {
            std::vector<std::jthread> threads;
            for (Index worker = 1; worker < workers; ++worker)
            {
                threads.emplace_back(work, worker);
            }
            work(0);
        }
        if (failure)
        {
            std::rethrow_exception(failure);
        }
        control.report(1.0);
        return table;
    }

    BootstrapInterval percentile_interval(std::span<const Real> replications, Real confidence)
    {
        if (replications.size() < 2)
        {
            throw Error("a bootstrap interval needs at least 2 replications");
        }
        if (!(confidence > 0.0 && confidence < 1.0))
        {
            throw Error("the confidence level must be strictly between 0 and 1");
        }
        const Real nan = std::numeric_limits<Real>::quiet_NaN();
        std::vector<Real> sorted(replications.begin(), replications.end());
        for (const Real x : sorted)
        {
            if (!std::isfinite(x))
            {
                return {nan, nan, nan};
            }
        }
        std::sort(sorted.begin(), sorted.end());
        const auto quantile = [&](Real p)
        {
            const Real position = p * static_cast<Real>(sorted.size() - 1);
            const Index below = static_cast<Index>(std::floor(position));
            const Index above = std::min(below + 1, sorted.size() - 1);
            const Real weight = position - static_cast<Real>(below);
            return sorted[below] + weight * (sorted[above] - sorted[below]);
        };
        Real sum = 0.0;
        for (const Real x : sorted)
        {
            sum += x;
        }
        const Real center = sum / static_cast<Real>(sorted.size());
        Real squares = 0.0;
        for (const Real x : sorted)
        {
            squares += (x - center) * (x - center);
        }
        return {quantile(0.5 * (1.0 - confidence)), quantile(0.5 * (1.0 + confidence)),
                std::sqrt(squares / static_cast<Real>(sorted.size() - 1))};
    }

} // namespace quantBacktest
