#ifndef QUANTBACKTEST_CORE_FINGERPRINT_HPP
#define QUANTBACKTEST_CORE_FINGERPRINT_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace quantBacktest
{

    /// SHA-256 (FIPS 180-4), fed in pieces. It is the fingerprint of the
    /// arrays a run was given: what the caller records as "the data that were
    /// read" to replay the run (docs/contract.md, section 4).
    class Fingerprint
    {
      public:
        Fingerprint() noexcept;

        void update(std::span<const std::byte> bytes) noexcept;
        void update(const void *data, std::size_t size) noexcept;
        /// Eight bytes, least significant first, on every platform.
        void update(std::uint64_t value) noexcept;

        /// The digest as 64 lowercase hexadecimal digits. The object must not
        /// be fed afterwards.
        std::string hex() noexcept;

      private:
        void compress(const std::uint8_t *block) noexcept;

        std::array<std::uint32_t, 8> state_;
        std::array<std::uint8_t, 64> buffer_;
        std::size_t buffered_ = 0;
        std::uint64_t length_ = 0;
    };

} // namespace quantBacktest

#endif
