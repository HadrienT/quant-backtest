#include "quantBacktest/core/parallel.hpp"

#include <algorithm>
#include <atomic>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

namespace quantBacktest
{

    void parallel_for(Index count, Index threads, const RunControl &control, const std::function<void(Index)> &task)
    {
        if (threads == 0)
        {
            throw Error("the number of threads must be at least 1");
        }
        if (count == 0)
        {
            return;
        }
        const Index workers = std::min(threads, count);
        std::atomic<Index> done{0};
        std::atomic<bool> stop{false};
        std::exception_ptr failure;
        std::mutex failureMutex;

        // Worker w takes k = w, w + workers, w + 2 workers, ...
        const auto work = [&](Index worker)
        {
            try
            {
                for (Index k = worker; k < count && !stop.load(std::memory_order_relaxed); k += workers)
                {
                    control.check_cancelled();
                    task(k);
                    const Index finished = done.fetch_add(1, std::memory_order_relaxed) + 1;
                    if (worker == 0)
                    {
                        control.report(static_cast<double>(finished) / static_cast<double>(count));
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
            std::vector<std::jthread> pool;
            for (Index worker = 1; worker < workers; ++worker)
            {
                pool.emplace_back(work, worker);
            }
            work(0);
        }
        if (failure)
        {
            std::rethrow_exception(failure);
        }
        control.report(1.0);
    }

} // namespace quantBacktest
