#include "quantBacktest/core/types.hpp"

#include <chrono>
#include <cstdio>

namespace quantBacktest
{

    std::string format_date(Date date)
    {
        const std::chrono::year_month_day ymd{std::chrono::sys_days{std::chrono::days{date}}};
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%04d-%02u-%02u",
                      static_cast<int>(ymd.year()),
                      static_cast<unsigned>(ymd.month()),
                      static_cast<unsigned>(ymd.day()));
        return buffer;
    }

} // namespace quantBacktest
