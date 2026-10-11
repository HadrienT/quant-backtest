#ifndef QUANTBACKTEST_CORE_TYPES_HPP
#define QUANTBACKTEST_CORE_TYPES_HPP

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace quantBacktest
{

    using Real = double;
    using Index = std::size_t;

    /// A calendar day, counted in days since 1970-01-01: the integer behind a
    /// numpy `datetime64[D]` (docs/contract.md, section 3).
    using Date = std::int32_t;

    inline constexpr Index npos = static_cast<Index>(-1);

    /// "YYYY-MM-DD", for messages.
    std::string format_date(Date date);

    /// A missing value is a NaN: "no quote that day". It is never replaced by
    /// an interpolation.
    inline bool is_missing(Real x) noexcept
    {
        return std::isnan(x);
    }

    /// Non-owning view of a row-major table, one row per date and one column
    /// per asset. A default-constructed view means "this table was not given".
    class MatrixView
    {
      public:
        MatrixView() = default;
        MatrixView(const Real *data, Index rows, Index cols) noexcept
            : data_(data), rows_(rows), cols_(cols) {}

        bool empty() const noexcept { return data_ == nullptr; }
        Index rows() const noexcept { return rows_; }
        Index cols() const noexcept { return cols_; }
        const Real *data() const noexcept { return data_; }

        Real operator()(Index row, Index col) const noexcept { return data_[row * cols_ + col]; }

        std::span<const Real> row(Index r) const noexcept { return {data_ + r * cols_, cols_}; }

        /// The first `n` rows; an empty view stays empty.
        MatrixView first_rows(Index n) const noexcept
        {
            return empty() ? MatrixView{} : MatrixView{data_, n, cols_};
        }

      private:
        const Real *data_ = nullptr;
        Index rows_ = 0;
        Index cols_ = 0;
    };

} // namespace quantBacktest

#endif
