#ifndef QUANTBACKTEST_CORE_RANDOM_HPP
#define QUANTBACKTEST_CORE_RANDOM_HPP

#include "quantBacktest/core/types.hpp"

#include <cstdint>

namespace quantBacktest
{

    /// The engine's own generator: the same seed gives the same draws on every
    /// platform and standard library, which `std::` distributions do not
    /// promise. It is the condition of a replay to the bit.
    ///
    /// SplitMix64 (Steele, Lea and Flood, "Fast Splittable Pseudorandom Number
    /// Generators", OOPSLA 2014). A stream is a (seed, stream) pair: giving
    /// each bootstrap replication its own stream makes the result independent
    /// of the number of threads.
    class Random
    {
      public:
        explicit Random(std::uint64_t seed, std::uint64_t stream = 0) noexcept
            : state_(mix(seed) ^ mix(stream * 0x9E3779B97F4A7C15ull + 0xD1B54A32D192ED03ull))
        {
        }

        std::uint64_t next() noexcept
        {
            state_ += 0x9E3779B97F4A7C15ull;
            return mix(state_);
        }

        /// Uniform on [0, 1), with 53 random bits.
        Real uniform() noexcept
        {
            return static_cast<Real>(next() >> 11) * 0x1.0p-53;
        }

        /// Uniform on {0, ..., n - 1}; n must be positive.
        Index uniform_index(Index n) noexcept
        {
            return static_cast<Index>(uniform() * static_cast<Real>(n));
        }

      private:
        static std::uint64_t mix(std::uint64_t z) noexcept
        {
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
            return z ^ (z >> 31);
        }

        std::uint64_t state_;
    };

} // namespace quantBacktest

#endif
