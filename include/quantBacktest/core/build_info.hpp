#ifndef QUANTBACKTEST_CORE_BUILD_INFO_HPP
#define QUANTBACKTEST_CORE_BUILD_INFO_HPP

#include <string_view>

namespace quantBacktest
{

    /// What a caller records to replay a run (docs/contract.md, section 4).
    struct BuildInfo
    {
        /// Version of this library, "major.minor.patch".
        std::string_view version;
        /// Commit of quant-backtest the library was built from: 40 hex digits,
        /// followed by "-dirty" if the tree had uncommitted changes, or
        /// "unknown" if the build had no git information.
        std::string_view git_sha;
        /// Commit of quantModeling the hedging module was built against: 40 hex
        /// digits, or empty if the library was built without hedging.
        std::string_view quantmodeling_sha;
    };

    const BuildInfo &build_info() noexcept;

} // namespace quantBacktest

#endif
