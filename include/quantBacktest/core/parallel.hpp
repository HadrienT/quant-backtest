#ifndef QUANTBACKTEST_CORE_PARALLEL_HPP
#define QUANTBACKTEST_CORE_PARALLEL_HPP

#include "quantBacktest/core/run_control.hpp"
#include "quantBacktest/core/types.hpp"

#include <functional>

namespace quantBacktest
{

    /// Calls `task(k)` for k = 0 ... count - 1 on `threads` threads, the
    /// calling one included. Tasks must be independent and write only to what
    /// belongs to their own k: the result then does not depend on the number
    /// of threads.
    ///
    /// Progress is reported from the calling thread only. A task that throws,
    /// or a cancellation, stops the others; the first exception is rethrown.
    void parallel_for(Index count, Index threads, const RunControl &control, const std::function<void(Index)> &task);

} // namespace quantBacktest

#endif
