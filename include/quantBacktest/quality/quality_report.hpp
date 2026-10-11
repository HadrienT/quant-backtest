#ifndef QUANTBACKTEST_QUALITY_QUALITY_REPORT_HPP
#define QUANTBACKTEST_QUALITY_QUALITY_REPORT_HPP

#include "quantBacktest/core/types.hpp"
#include "quantBacktest/data/market_data.hpp"

#include <string>
#include <vector>

namespace quantBacktest
{

    /// Controls of the companion, section 3.6.
    enum class QualityCode
    {
        // Errors: the series cannot be used.
        EmptyData,
        ShapeMismatch,
        DatesNotIncreasing,
        InvalidPrice,  ///< zero, negative or infinite
        InvalidVolume, ///< negative or infinite
        HighBelowLow,
        InvalidRate,  ///< missing or infinite
        InvalidEvent, ///< unknown asset, non-positive amount or ratio
        // Warnings: the series can be used, and the reader must know.
        CloseOutsideRange, ///< close or open outside [low, high]
        ZeroVolume,
        ExtremeReturn, ///< often a split that is missing from the events
        SeriesEnds,    ///< no quote from some date to the end
        MissingQuotes, ///< days without a quote inside the life of the series
        NeverQuoted,
        StalePrices,      ///< too many unchanged closes
        EventOffCalendar, ///< applied on the next date of the calendar
        WeekendDate,
    };

    enum class Severity
    {
        Warning,
        Error,
    };

    struct QualityIssue
    {
        QualityCode code;
        Severity severity;
        /// `npos` when the issue is not about one asset.
        Index asset;
        /// Number of occurrences, and the date of the first one.
        Index count;
        Date first_date;
        /// In English, ready to be shown.
        std::string message;
    };

    struct QualityThresholds
    {
        /// A split-adjusted price return is extreme beyond this many robust
        /// standard deviations (1.4826 times the median absolute deviation)
        /// from the median.
        Real extreme_return_deviations = 20.0;
        /// Prices are stale when more than this fraction of the returns is
        /// exactly zero.
        Real stale_fraction = 0.2;
        /// Relative tolerance on "close within [low, high]".
        Real range_tolerance = 1e-6;
    };

    struct QualityReport
    {
        std::vector<QualityIssue> issues;

        /// False as soon as one issue is an error.
        bool usable() const noexcept;
        Index count(QualityCode code) const noexcept;
        /// The errors, one per line.
        std::string error_summary() const;
    };

    /// Runs every control. It never throws on bad data: an unusable series is
    /// a report with errors. When the shapes are wrong the other controls are
    /// skipped, as they could not be run.
    QualityReport check_market_data(const MarketData &market, const QualityThresholds &thresholds = {});

    /// The controls that do not read the price tables: shapes, dates, rates
    /// and events. What is left to verify, at a negligible cost, on data that
    /// already went through `check_market_data`.
    QualityReport check_market_structure(const MarketData &market);

} // namespace quantBacktest

#endif
