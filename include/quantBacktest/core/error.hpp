#ifndef QUANTBACKTEST_CORE_ERROR_HPP
#define QUANTBACKTEST_CORE_ERROR_HPP

#include <stdexcept>
#include <string>

namespace quantBacktest
{

    /// An input the engine cannot use. The message is in English and can be
    /// shown to the user as it is (docs/contract.md, section 3).
    class Error : public std::runtime_error
    {
      public:
        explicit Error(const std::string &message)
            : std::runtime_error(message) {}
    };

    /// The caller raised the cancellation signal of a long call.
    class Cancelled : public Error
    {
      public:
        Cancelled()
            : Error("the run was cancelled") {}
    };

} // namespace quantBacktest

#endif
