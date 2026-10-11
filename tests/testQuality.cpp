#include <gtest/gtest.h>

#include "quantBacktest/quality/quality_report.hpp"

#include "support/synthetic_market.hpp"

#include <limits>

namespace quantBacktest
{

    namespace
    {

        constexpr Real NaN = std::numeric_limits<Real>::quiet_NaN();

        testing::SyntheticMarket cleanMarket()
        {
            return testing::makeMarket(500, 4, 7);
        }

        QualityReport check(const testing::SyntheticMarket &m)
        {
            return check_market_data(m.data());
        }

    } // namespace

    TEST(Quality, CleanDataHasNoIssue)
    {
        const QualityReport report = check(cleanMarket());
        EXPECT_TRUE(report.usable());
        for (const QualityIssue &issue : report.issues)
        {
            ADD_FAILURE() << issue.message;
        }
    }

    TEST(Quality, EmptyOrMisshapenDataIsRefused)
    {
        testing::SyntheticMarket m = cleanMarket();
        m.close.pop_back();
        MarketData data = m.data();
        data.close = {m.close.data(), m.days - 1, m.assets};
        EXPECT_EQ(check_market_data(data).count(QualityCode::ShapeMismatch), 1u);
        EXPECT_FALSE(check_market_data(data).usable());

        EXPECT_EQ(check_market_data(MarketData{}).count(QualityCode::EmptyData), 1u);

        m = cleanMarket();
        m.cashRate.pop_back();
        EXPECT_EQ(check(m).count(QualityCode::ShapeMismatch), 1u);
    }

    TEST(Quality, DuplicateOrUnsortedDatesAreRefused)
    {
        testing::SyntheticMarket m = cleanMarket();
        m.dates[10] = m.dates[9];
        const QualityReport report = check(m);
        EXPECT_FALSE(report.usable());
        EXPECT_EQ(report.count(QualityCode::DatesNotIncreasing), 1u);
    }

    TEST(Quality, NonPositiveOrInfinitePricesAreRefused)
    {
        testing::SyntheticMarket m = cleanMarket();
        m.close[m.at(3, 1)] = 0.0;
        m.open[m.at(4, 1)] = -1.0;
        m.close[m.at(5, 2)] = std::numeric_limits<Real>::infinity();
        const QualityReport report = check(m);
        EXPECT_FALSE(report.usable());
        EXPECT_EQ(report.count(QualityCode::InvalidPrice), 3u);
        EXPECT_NE(report.error_summary().find("asset 1"), std::string::npos);
    }

    TEST(Quality, HighBelowLowIsRefusedAndCloseOutsideRangeIsFlagged)
    {
        testing::SyntheticMarket m = cleanMarket();
        std::swap(m.high[m.at(8, 0)], m.low[m.at(8, 0)]);
        EXPECT_EQ(check(m).count(QualityCode::HighBelowLow), 1u);
        EXPECT_FALSE(check(m).usable());

        m = cleanMarket();
        m.close[m.at(8, 0)] = m.high[m.at(8, 0)] * 1.01;
        const QualityReport report = check(m);
        EXPECT_TRUE(report.usable());
        EXPECT_EQ(report.count(QualityCode::CloseOutsideRange), 1u);
    }

    TEST(Quality, VolumeControls)
    {
        testing::SyntheticMarket m = cleanMarket();
        m.volume[m.at(8, 0)] = 0.0;
        m.volume[m.at(9, 0)] = 0.0;
        EXPECT_EQ(check(m).count(QualityCode::ZeroVolume), 2u);
        EXPECT_TRUE(check(m).usable());
        m.volume[m.at(9, 0)] = -5.0;
        EXPECT_EQ(check(m).count(QualityCode::InvalidVolume), 1u);
        EXPECT_FALSE(check(m).usable());
    }

    TEST(Quality, MissingRateIsRefused)
    {
        testing::SyntheticMarket m = cleanMarket();
        m.cashRate[20] = NaN;
        EXPECT_EQ(check(m).count(QualityCode::InvalidRate), 1u);
        EXPECT_FALSE(check(m).usable());
    }

    TEST(Quality, InvalidEventsAreRefusedAndOffCalendarOnesAreFlagged)
    {
        testing::SyntheticMarket m = cleanMarket();
        m.dividends.push_back({0, m.dates[5], -1.0});
        m.splits.push_back({1, m.dates[5], 0.0});
        m.splits.push_back({99, m.dates[5], 2.0});
        const QualityReport bad = check(m);
        EXPECT_FALSE(bad.usable());
        EXPECT_EQ(bad.count(QualityCode::InvalidEvent), 3u);

        m = cleanMarket();
        // Saturday 2000-01-08 is not in the calendar.
        m.dividends.push_back({0, 10964, 0.1});
        const QualityReport report = check(m);
        EXPECT_TRUE(report.usable());
        EXPECT_EQ(report.count(QualityCode::EventOffCalendar), 1u);
    }

    // A spin-off recorded both as a dividend and as a split (seven such days
    // in the S&P 500 history loaded by data-ingest) is flagged for the reader.
    TEST(Quality, DividendAndSplitOnTheSameDayAreFlagged)
    {
        testing::SyntheticMarket m = cleanMarket();
        m.addSplit(1, 200, 2.0);
        m.dividends.push_back({1, m.dates[200], 0.5});
        m.dividends.push_back({2, m.dates[200], 0.5});
        const QualityReport report = check(m);
        EXPECT_TRUE(report.usable());
        ASSERT_EQ(report.count(QualityCode::DividendAndSplitSameDay), 1u);
        for (const QualityIssue &issue : report.issues)
        {
            if (issue.code == QualityCode::DividendAndSplitSameDay)
            {
                EXPECT_EQ(issue.asset, 1u);
                EXPECT_EQ(issue.first_date, m.dates[200]);
            }
        }
    }

    // An extreme return is most often a split missing from the events: once
    // the split is declared, the same prices are clean.
    TEST(Quality, UndeclaredSplitIsAnExtremeReturn)
    {
        testing::SyntheticMarket m = cleanMarket();
        m.addSplit(2, 250, 4.0);
        EXPECT_EQ(check(m).count(QualityCode::ExtremeReturn), 0u);

        m.splits.clear();
        const QualityReport report = check(m);
        EXPECT_TRUE(report.usable());
        ASSERT_EQ(report.count(QualityCode::ExtremeReturn), 1u);
        for (const QualityIssue &issue : report.issues)
        {
            if (issue.code == QualityCode::ExtremeReturn)
            {
                EXPECT_EQ(issue.asset, 2u);
                EXPECT_EQ(issue.first_date, m.dates[250]);
            }
        }
    }

    TEST(Quality, GapsEndsAndUnquotedSeriesAreFlagged)
    {
        testing::SyntheticMarket m = cleanMarket();
        // Asset 0 is listed late (not an issue), asset 1 has a gap, asset 2
        // stops, asset 3 never quotes.
        for (Index t = 0; t < m.days; ++t)
        {
            for (std::vector<Real> *table : {&m.open, &m.high, &m.low, &m.close})
            {
                if (t < 30)
                {
                    (*table)[m.at(t, 0)] = NaN;
                }
                if (t == 100 || t == 101)
                {
                    (*table)[m.at(t, 1)] = NaN;
                }
                if (t >= 400)
                {
                    (*table)[m.at(t, 2)] = NaN;
                }
                (*table)[m.at(t, 3)] = NaN;
            }
        }
        const QualityReport report = check(m);
        EXPECT_TRUE(report.usable());
        EXPECT_EQ(report.count(QualityCode::MissingQuotes), 2u);
        EXPECT_EQ(report.count(QualityCode::SeriesEnds), 1u);
        EXPECT_EQ(report.count(QualityCode::NeverQuoted), 1u);
        for (const QualityIssue &issue : report.issues)
        {
            if (issue.code == QualityCode::SeriesEnds)
            {
                EXPECT_EQ(issue.asset, 2u);
                EXPECT_EQ(issue.first_date, m.dates[400]);
            }
        }
    }

    TEST(Quality, FrozenPricesAreFlagged)
    {
        testing::SyntheticMarket m = cleanMarket();
        for (Index t = 100; t < 300; ++t)
        {
            for (std::vector<Real> *table : {&m.open, &m.high, &m.low, &m.close})
            {
                (*table)[m.at(t, 1)] = (*table)[m.at(99, 1)];
            }
        }
        const QualityReport report = check(m);
        EXPECT_EQ(report.count(QualityCode::StalePrices), 1u);
    }

    // The cheap controls still refuse what would make the engine read out of
    // bounds, and skip what reads the prices.
    TEST(Quality, StructureControlsDoNotReadPrices)
    {
        testing::SyntheticMarket m = cleanMarket();
        m.close[m.at(3, 1)] = -1.0;
        EXPECT_TRUE(check_market_structure(m.data()).usable());
        EXPECT_FALSE(check(m).usable());

        m.splits.push_back({99, m.dates[5], 2.0});
        EXPECT_FALSE(check_market_structure(m.data()).usable());
        m.splits.clear();
        m.dates[10] = m.dates[9];
        EXPECT_FALSE(check_market_structure(m.data()).usable());
        m = cleanMarket();
        m.cashRate.pop_back();
        EXPECT_FALSE(check_market_structure(m.data()).usable());
    }

    TEST(Quality, WeekendDatesAreFlagged)
    {
        testing::SyntheticMarket m = cleanMarket();
        // 2000-01-08 and 2000-01-09, between Friday the 7th and Monday the 10th.
        m.dates[4] = 10964;
        m.dates[5] = 10965;
        // Keep the calendar increasing: shift the rest by a week.
        for (Index t = 6; t < m.days; ++t)
        {
            m.dates[t] += 7;
        }
        m.dividends.clear();
        EXPECT_EQ(check(m).count(QualityCode::WeekendDate), 2u);
    }

} // namespace quantBacktest
