#ifndef QUANTBACKTEST_CORE_RUN_CONTROL_HPP
#define QUANTBACKTEST_CORE_RUN_CONTROL_HPP

#include "quantBacktest/core/error.hpp"

#include <atomic>
#include <functional>

namespace quantBacktest
{

    /// How a long call reports its progress and is asked to stop. Managing the
    /// task (queue, identifier, user cancellation) belongs to the caller.
    struct RunControl
    {
        /// Called with the completed fraction, in [0, 1], non-decreasing.
        std::function<void(double)> progress;
        /// Set to true by the caller, from any thread, to stop the run. The
        /// call then throws `Cancelled`.
        const std::atomic<bool> *cancel = nullptr;

        void check_cancelled() const
        {
            if (cancel != nullptr && cancel->load(std::memory_order_relaxed))
            {
                throw Cancelled();
            }
        }

        void report(double fraction) const
        {
            if (progress)
            {
                progress(fraction);
            }
        }
    };

} // namespace quantBacktest

#endif
