#ifndef QUANTBACKTEST_DATA_MARKET_DATA_HPP
#define QUANTBACKTEST_DATA_MARKET_DATA_HPP

#include "quantBacktest/core/types.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace quantBacktest
{

    /// A cash dividend. `amount` is per share, in the share count of the
    /// ex-date: if a split takes effect the same day, per post-split share.
    struct Dividend
    {
        Index asset = 0;
        Date ex_date = 0;
        Real amount = 0.0;
    };

    /// A stock split. `ratio` is the number of new shares per old share (2 for
    /// a two-for-one split, 0.1 for a one-for-ten reverse split). Raw prices
    /// are quoted in the new share count from `date` on.
    struct Split
    {
        Index asset = 0;
        Date date = 0;
        Real ratio = 1.0;
    };

    /// The arrays received from the caller. Nothing is owned and nothing is
    /// read from anywhere else (ADR-007).
    ///
    /// Tables have one row per date and one column per asset. Prices are raw:
    /// as quoted that day, adjusted for nothing (companion, section 3.3).
    struct MarketData
    {
        /// Strictly increasing. The calendar is the one of the data received.
        std::span<const Date> dates;
        Index assets = 0;

        /// Required.
        MatrixView close;
        /// Optional; `open` is required to execute at the open, `high` and
        /// `low` by the spread estimators.
        MatrixView open;
        MatrixView high;
        MatrixView low;
        MatrixView volume;

        /// Optional, row-major like the tables: non-zero where the asset
        /// belongs to the investable universe on that date.
        std::span<const std::uint8_t> universe;

        /// Optional, one value per date: the simple rate earned by a positive
        /// cash balance over the period that ends on that date, as it was
        /// known at the previous date (see `period_rates_from_annualized`).
        /// Without it, cash earns nothing.
        std::span<const Real> cash_rate;
        /// Optional: the same for a negative balance. Without it, a negative
        /// balance pays `cash_rate`.
        std::span<const Real> debit_rate;

        std::span<const Dividend> dividends;
        std::span<const Split> splits;
    };

    /// Corporate actions sorted by the day they take effect.
    ///
    /// An event dated between two dates of the calendar takes effect on the
    /// first date that follows it; an event dated outside the calendar is
    /// dropped. Throws if the dates are not strictly increasing or if an event
    /// names an asset that does not exist.
    class EventIndex
    {
      public:
        explicit EventIndex(const MarketData &market);

        std::span<const Dividend> dividends_on(Index day) const noexcept;
        std::span<const Split> splits_on(Index day) const noexcept;

        /// Everything that took effect on or before `day`.
        std::span<const Dividend> dividends_through(Index day) const noexcept;
        std::span<const Split> splits_through(Index day) const noexcept;

        /// Product of the split ratios of `asset` taking effect on `day`
        /// (1 if none), and sum of its dividends that day (0 if none).
        Real split_ratio(Index day, Index asset) const noexcept;
        Real dividend(Index day, Index asset) const noexcept;

      private:
        std::vector<Dividend> dividends_;
        std::vector<Index> dividend_offsets_;
        std::vector<Split> splits_;
        std::vector<Index> split_offsets_;
    };

    /// What was known at the close of one day: every accessor refuses a row
    /// after that day, and the spans it hands out stop there. This is the only
    /// way a strategy or a cost model reads the market (companion, section
    /// 3.2).
    class MarketView
    {
      public:
        MarketView(const MarketData &market, const EventIndex &events, Index day);

        /// Index of the last visible day, and number of assets.
        Index day() const noexcept { return day_; }
        Index assets() const noexcept { return assets_; }

        std::span<const Date> dates() const noexcept { return dates_; }
        Date date(Index s) const { return dates_[checked(s)]; }

        bool has_open() const noexcept { return !open_.empty(); }
        bool has_high_low() const noexcept { return !high_.empty() && !low_.empty(); }
        bool has_volume() const noexcept { return !volume_.empty(); }

        Real close(Index s, Index asset) const { return close_(checked(s), asset); }
        Real open(Index s, Index asset) const { return required(open_, "open")(checked(s), asset); }
        Real high(Index s, Index asset) const { return required(high_, "high")(checked(s), asset); }
        Real low(Index s, Index asset) const { return required(low_, "low")(checked(s), asset); }
        Real volume(Index s, Index asset) const { return required(volume_, "volume")(checked(s), asset); }

        std::span<const Real> close_row(Index s) const { return close_.row(checked(s)); }

        /// True when no universe was given.
        bool in_universe(Index s, Index asset) const;

        std::span<const Dividend> dividends() const noexcept { return events_->dividends_through(day_); }
        std::span<const Split> splits() const noexcept { return events_->splits_through(day_); }
        Real split_ratio(Index s, Index asset) const { return events_->split_ratio(checked(s), asset); }
        Real dividend(Index s, Index asset) const { return events_->dividend(checked(s), asset); }

      private:
        Index checked(Index s) const;
        static const MatrixView &required(const MatrixView &table, const char *name);

        Index day_;
        Index assets_;
        std::span<const Date> dates_;
        MatrixView close_;
        MatrixView open_;
        MatrixView high_;
        MatrixView low_;
        MatrixView volume_;
        std::span<const std::uint8_t> universe_;
        const EventIndex *events_;
    };

} // namespace quantBacktest

#endif
