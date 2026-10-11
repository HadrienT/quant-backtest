#include <gtest/gtest.h>

#include "quantBacktest/core/error.hpp"
#include "quantBacktest/data/market_data.hpp"
#include "quantBacktest/data/returns.hpp"

#include "support/synthetic_market.hpp"

#include <cmath>
#include <limits>
#include <vector>

namespace quantBacktest
{

    namespace
    {

        constexpr Real NaN = std::numeric_limits<Real>::quiet_NaN();

        // Four days (Monday to Thursday), two assets.
        testing::SyntheticMarket tinyMarket()
        {
            testing::SyntheticMarket m;
            m.days = 4;
            m.assets = 2;
            m.dates = {10959, 10960, 10961, 10962};
            m.close = {100.0, 50.0,
                       110.0, 50.0,
                       55.0, NaN,
                       66.0, 52.0};
            return m;
        }

    } // namespace

    TEST(Types, FormatDate)
    {
        EXPECT_EQ(format_date(0), "1970-01-01");
        EXPECT_EQ(format_date(10959), "2000-01-03");
        EXPECT_EQ(format_date(-1), "1969-12-31");
    }

    TEST(TotalReturns, DividendAndSplitByHand)
    {
        testing::SyntheticMarket m = tinyMarket();
        // Asset 0: a dividend of 1 on day 1, a two-for-one split on day 2 with
        // a dividend of 0.5 per new share the same day.
        m.dividends = {{0, 10960, 1.0}, {0, 10961, 0.5}};
        m.splits = {{0, 10961, 2.0}};
        const std::vector<Real> r = total_returns(m.data());

        EXPECT_TRUE(std::isnan(r[0]));
        EXPECT_TRUE(std::isnan(r[1]));
        EXPECT_DOUBLE_EQ(r[m.at(1, 0)], (110.0 + 1.0) / 100.0 - 1.0);
        EXPECT_DOUBLE_EQ(r[m.at(2, 0)], 2.0 * (55.0 + 0.5) / 110.0 - 1.0);
        EXPECT_DOUBLE_EQ(r[m.at(3, 0)], 66.0 / 55.0 - 1.0);
        EXPECT_DOUBLE_EQ(r[m.at(1, 1)], 0.0);
    }

    // Finding 2 of the companion: a return is never taken across a gap, and
    // the gap is not filled.
    TEST(TotalReturns, NoReturnAcrossAMissingQuote)
    {
        const testing::SyntheticMarket m = tinyMarket();
        const std::vector<Real> r = total_returns(m.data());
        EXPECT_TRUE(std::isnan(r[m.at(2, 1)]));
        EXPECT_TRUE(std::isnan(r[m.at(3, 1)]));
    }

    TEST(PeriodRates, UsesTheRateKnownAtTheStartOfThePeriod)
    {
        // Friday, Monday, Tuesday.
        const std::vector<Date> dates = {10963, 10966, 10967};
        const std::vector<Real> annual = {0.036, 0.072, 0.5};
        const std::vector<Real> rates = period_rates_from_annualized(dates, annual, 360.0);
        EXPECT_DOUBLE_EQ(rates[0], 0.0);
        EXPECT_DOUBLE_EQ(rates[1], 0.036 * 3.0 / 360.0);
        EXPECT_DOUBLE_EQ(rates[2], 0.072 * 1.0 / 360.0);
        EXPECT_THROW(period_rates_from_annualized(dates, std::vector<Real>{0.1}, 360.0), Error);
    }

    TEST(EventIndex, OffCalendarEventsMoveToTheNextDateAndOutsideOnesAreDropped)
    {
        testing::SyntheticMarket m = tinyMarket();
        m.dates = {10959, 10960, 10963, 10966};
        m.dividends = {
            {0, 10962, 1.0},  // between two dates: applied on 10963
            {1, 10950, 2.0},  // before the calendar: dropped
            {1, 10999, 3.0},  // after the calendar: dropped
            {0, 10963, 0.25}, // on a date
        };
        const MarketData data = m.data();
        const EventIndex events(data);
        EXPECT_EQ(events.dividends_on(0).size(), 0u);
        EXPECT_EQ(events.dividends_on(1).size(), 0u);
        EXPECT_EQ(events.dividends_on(2).size(), 2u);
        EXPECT_DOUBLE_EQ(events.dividend(2, 0), 1.25);
        EXPECT_DOUBLE_EQ(events.dividend(2, 1), 0.0);
        EXPECT_EQ(events.dividends_through(1).size(), 0u);
        EXPECT_EQ(events.dividends_through(3).size(), 2u);
        EXPECT_DOUBLE_EQ(events.split_ratio(2, 0), 1.0);
    }

    TEST(EventIndex, RefusesUnsortedDatesAndUnknownAssets)
    {
        testing::SyntheticMarket m = tinyMarket();
        m.dividends = {{2, 10960, 1.0}};
        EXPECT_THROW(EventIndex{m.data()}, Error);
        m.dividends.clear();
        m.dates = {10959, 10959, 10961, 10962};
        EXPECT_THROW(EventIndex{m.data()}, Error);
    }

    // Point-in-time by construction: a view cannot be read past its day.
    TEST(MarketView, RefusesToReadTheFuture)
    {
        testing::SyntheticMarket m = tinyMarket();
        m.dividends = {{0, 10960, 1.0}, {0, 10962, 2.0}};
        const MarketData data = m.data();
        const EventIndex events(data);
        const MarketView view(data, events, 1);

        EXPECT_DOUBLE_EQ(view.close(1, 0), 110.0);
        EXPECT_THROW(view.close(2, 0), Error);
        EXPECT_THROW(view.date(2), Error);
        EXPECT_THROW(view.in_universe(2, 0), Error);
        EXPECT_THROW(view.split_ratio(2, 0), Error);
        EXPECT_EQ(view.dates().size(), 2u);
        EXPECT_EQ(view.close_row(1).size(), 2u);
        // Only the dividend already detached is visible.
        ASSERT_EQ(view.dividends().size(), 1u);
        EXPECT_DOUBLE_EQ(view.dividends()[0].amount, 1.0);
    }

    TEST(MarketView, MissingTableIsAnError)
    {
        const testing::SyntheticMarket m = tinyMarket();
        const MarketData data = m.data();
        const EventIndex events(data);
        const MarketView view(data, events, 3);
        EXPECT_FALSE(view.has_open());
        EXPECT_THROW(view.open(0, 0), Error);
        EXPECT_TRUE(view.in_universe(0, 0));
    }

} // namespace quantBacktest
