#include <gtest/gtest.h>

#include "quantBacktest/core/build_info.hpp"

#include <algorithm>
#include <cctype>
#include <string_view>

namespace quantBacktest
{

    namespace
    {

        bool isCommitSha(std::string_view s)
        {
            return s.size() == 40 && std::all_of(s.begin(), s.end(), [](unsigned char c)
                                                 { return std::isxdigit(c) != 0; });
        }

    } // namespace

    TEST(BuildInfo, VersionIsThreeNumbers)
    {
        const std::string_view version = build_info().version;
        ASSERT_FALSE(version.empty());
        EXPECT_EQ(std::count(version.begin(), version.end(), '.'), 2);
        EXPECT_TRUE(std::all_of(version.begin(), version.end(), [](unsigned char c)
                                { return std::isdigit(c) != 0 || c == '.'; }));
    }

    TEST(BuildInfo, GitShaIsACommitOrUnknown)
    {
        std::string_view sha = build_info().git_sha;
        if (sha == "unknown")
        {
            return;
        }
        constexpr std::string_view dirty = "-dirty";
        if (sha.ends_with(dirty))
        {
            sha.remove_suffix(dirty.size());
        }
        EXPECT_TRUE(isCommitSha(sha)) << build_info().git_sha;
    }

    // The pin is a commit, never a branch name (ADR-003).
    TEST(BuildInfo, QuantModelingPinIsACommitOrEmpty)
    {
        const std::string_view sha = build_info().quantmodeling_sha;
        EXPECT_TRUE(sha.empty() || isCommitSha(sha)) << sha;
    }

} // namespace quantBacktest
