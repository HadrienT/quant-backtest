#include "quantBacktest/quality/quality_report.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <utility>

namespace quantBacktest
{

    namespace
    {

        // Occurrences of one kind of issue on one asset, merged into a single
        // line of the report.
        class Collector
        {
          public:
            // `date` is the date of the first of the `count` occurrences added.
            void add(QualityCode code, Severity severity, Index asset, Date date, const char *what, Index count = 1)
            {
                Entry &entry = entries_[{code, asset}];
                if (entry.count == 0)
                {
                    entry.severity = severity;
                    entry.first_date = date;
                    entry.what = what;
                }
                entry.count += count;
            }

            void addGlobal(QualityCode code, Severity severity, std::string message)
            {
                globals_.push_back({code, severity, npos, 1, 0, std::move(message)});
            }

            QualityReport report() const
            {
                QualityReport out;
                out.issues = globals_;
                for (const auto &[key, entry] : entries_)
                {
                    std::string message = key.second == npos ? std::string() : "asset " + std::to_string(key.second) + ": ";
                    message += entry.what;
                    message += " (" + std::to_string(entry.count) + (entry.count == 1 ? " occurrence, on " : " occurrences, first on ") + format_date(entry.first_date) + ")";
                    out.issues.push_back({key.first, entry.severity, key.second, entry.count, entry.first_date, std::move(message)});
                }
                return out;
            }

          private:
            struct Entry
            {
                Severity severity = Severity::Warning;
                Index count = 0;
                Date first_date = 0;
                const char *what = "";
            };
            std::map<std::pair<QualityCode, Index>, Entry> entries_;
            std::vector<QualityIssue> globals_;
        };

        bool validPrice(Real x)
        {
            return is_missing(x) || (std::isfinite(x) && x > 0.0);
        }

        Real median(std::vector<Real> &values)
        {
            const auto middle = values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2);
            std::nth_element(values.begin(), middle, values.end());
            return *middle;
        }

        bool checkShapes(const MarketData &market, Collector &out)
        {
            const Index days = market.dates.size();
            const Index assets = market.assets;
            if (days == 0 || assets == 0)
            {
                out.addGlobal(QualityCode::EmptyData, Severity::Error, "there is no date or no asset");
                return false;
            }
            bool ok = true;
            const auto table = [&](const MatrixView &view, const char *name, bool isRequired)
            {
                if (view.empty() ? isRequired : (view.rows() != days || view.cols() != assets))
                {
                    out.addGlobal(QualityCode::ShapeMismatch, Severity::Error,
                                  std::string("the '") + name + "' table must have one row per date and one column per asset");
                    ok = false;
                }
            };
            table(market.close, "close", true);
            table(market.open, "open", false);
            table(market.high, "high", false);
            table(market.low, "low", false);
            table(market.volume, "volume", false);
            if (market.high.empty() != market.low.empty())
            {
                out.addGlobal(QualityCode::ShapeMismatch, Severity::Error, "'high' and 'low' must be given together");
                ok = false;
            }
            const auto series = [&](Index size, Index expected, const char *name)
            {
                if (size != 0 && size != expected)
                {
                    out.addGlobal(QualityCode::ShapeMismatch, Severity::Error, std::string("'") + name + "' does not have the expected length");
                    ok = false;
                }
            };
            series(market.universe.size(), days * assets, "universe");
            series(market.cash_rate.size(), days, "cash_rate");
            series(market.debit_rate.size(), days, "debit_rate");
            series(market.borrow_rate.size(), days, "borrow_rate");
            return ok;
        }

        bool checkDates(const MarketData &market, Collector &out)
        {
            bool increasing = true;
            for (Index t = 0; t < market.dates.size(); ++t)
            {
                const Date date = market.dates[t];
                if (t > 0 && date <= market.dates[t - 1])
                {
                    out.add(QualityCode::DatesNotIncreasing, Severity::Error, npos, date, "dates must be strictly increasing, without duplicates");
                    increasing = false;
                }
                // 1970-01-01 was a Thursday.
                const int weekday = static_cast<int>(((date % 7) + 7 + 4) % 7);
                if (weekday == 0 || weekday == 6)
                {
                    out.add(QualityCode::WeekendDate, Severity::Warning, npos, date, "a date falls on a weekend");
                }
            }
            return increasing;
        }

        void checkRates(std::span<const Real> rates, std::span<const Date> dates, const char *what, Collector &out)
        {
            for (Index t = 0; t < rates.size(); ++t)
            {
                if (!std::isfinite(rates[t]))
                {
                    out.add(QualityCode::InvalidRate, Severity::Error, npos, dates[t], what);
                }
            }
        }

        void checkEvents(const MarketData &market, Collector &out)
        {
            const auto onCalendar = [&](Date date)
            {
                return date < market.dates.front() || date > market.dates.back() ||
                       std::binary_search(market.dates.begin(), market.dates.end(), date);
            };
            for (const Dividend &d : market.dividends)
            {
                if (d.asset >= market.assets)
                {
                    out.add(QualityCode::InvalidEvent, Severity::Error, npos, d.ex_date, "a dividend refers to an asset that does not exist");
                }
                else if (!(std::isfinite(d.amount) && d.amount > 0.0))
                {
                    out.add(QualityCode::InvalidEvent, Severity::Error, d.asset, d.ex_date, "a dividend amount is not a positive number");
                }
                else if (!onCalendar(d.ex_date))
                {
                    out.add(QualityCode::EventOffCalendar, Severity::Warning, d.asset, d.ex_date,
                            "an ex-dividend date is not in the calendar; the dividend is applied on the next date");
                }
            }
            // A dividend and a split on the same day are legitimate, and rare.
            // They are also how a spin-off shows up at sources that count it
            // twice, once as a dividend and once as a split: the reader checks.
            std::set<std::pair<Index, Date>> splitDays;
            for (const Split &s : market.splits)
            {
                splitDays.insert({s.asset, s.date});
            }
            for (const Dividend &d : market.dividends)
            {
                if (d.asset < market.assets && splitDays.count({d.asset, d.ex_date}) != 0)
                {
                    out.add(QualityCode::DividendAndSplitSameDay, Severity::Warning, d.asset, d.ex_date,
                            "a dividend and a split take effect on the same day: check that a spin-off is not counted twice");
                }
            }
            for (const Split &s : market.splits)
            {
                if (s.asset >= market.assets)
                {
                    out.add(QualityCode::InvalidEvent, Severity::Error, npos, s.date, "a split refers to an asset that does not exist");
                }
                else if (!(std::isfinite(s.ratio) && s.ratio > 0.0))
                {
                    out.add(QualityCode::InvalidEvent, Severity::Error, s.asset, s.date, "a split ratio is not a positive number");
                }
                else if (!onCalendar(s.date))
                {
                    out.add(QualityCode::EventOffCalendar, Severity::Warning, s.asset, s.date,
                            "a split date is not in the calendar; the split is applied on the next date");
                }
            }
        }

        void checkBars(const MarketData &market, const QualityThresholds &thresholds, Collector &out)
        {
            const Index days = market.dates.size();
            const bool hasRange = !market.high.empty() && !market.low.empty();
            for (Index t = 0; t < days; ++t)
            {
                const Date date = market.dates[t];
                for (Index i = 0; i < market.assets; ++i)
                {
                    const Real close = market.close(t, i);
                    for (const MatrixView *table : {&market.close, &market.open, &market.high, &market.low})
                    {
                        if (!table->empty() && !validPrice((*table)(t, i)))
                        {
                            out.add(QualityCode::InvalidPrice, Severity::Error, i, date, "a price is zero, negative or infinite");
                        }
                    }
                    if (!market.volume.empty())
                    {
                        const Real volume = market.volume(t, i);
                        if (!is_missing(volume) && !(std::isfinite(volume) && volume >= 0.0))
                        {
                            out.add(QualityCode::InvalidVolume, Severity::Error, i, date, "a volume is negative or infinite");
                        }
                        else if (volume == 0.0 && !is_missing(close))
                        {
                            out.add(QualityCode::ZeroVolume, Severity::Warning, i, date, "a quoted day has zero volume");
                        }
                    }
                    if (hasRange)
                    {
                        const Real high = market.high(t, i);
                        const Real low = market.low(t, i);
                        if (high < low)
                        {
                            out.add(QualityCode::HighBelowLow, Severity::Error, i, date, "the high is below the low");
                        }
                        else
                        {
                            const Real slack = thresholds.range_tolerance * high;
                            const auto outside = [&](Real x)
                            { return x > high + slack || x < low - slack; };
                            if (outside(close) || (!market.open.empty() && outside(market.open(t, i))))
                            {
                                out.add(QualityCode::CloseOutsideRange, Severity::Warning, i, date, "the close or the open is outside the day's range");
                            }
                        }
                    }
                }
            }
        }

        // Controls on each asset's own history. Needs valid dates and events.
        // The tables are read row by row, as they are stored.
        void checkHistories(const MarketData &market, const QualityThresholds &thresholds, Collector &out)
        {
            struct History
            {
                Index first = npos;
                Index last = npos;
                Index quoted = 0;
                Index firstGap = npos;
                Index zeros = 0;
                std::vector<Real> returns;
                std::vector<Index> days;
            };
            const Index days = market.dates.size();
            const Index assets = market.assets;
            const EventIndex events(market);
            std::vector<History> histories(assets);
            std::vector<Real> ratio(assets, 1.0);
            for (Index t = 0; t < days; ++t)
            {
                for (const Split &split : events.splits_on(t))
                {
                    ratio[split.asset] *= split.ratio;
                }
                for (Index i = 0; i < assets; ++i)
                {
                    History &h = histories[i];
                    const Real current = market.close(t, i);
                    if (is_missing(current))
                    {
                        if (h.first != npos && h.last + 1 == t)
                        {
                            h.firstGap = h.firstGap == npos ? t : h.firstGap;
                        }
                        continue;
                    }
                    if (h.first != npos && h.last + 1 == t)
                    {
                        // Split-adjusted price return over consecutive quotes.
                        const Real r = ratio[i] * current / market.close(t - 1, i) - 1.0;
                        h.zeros += r == 0.0 ? 1 : 0;
                        h.returns.push_back(r);
                        h.days.push_back(t);
                    }
                    h.first = h.first == npos ? t : h.first;
                    h.last = t;
                    ++h.quoted;
                }
                for (const Split &split : events.splits_on(t))
                {
                    ratio[split.asset] = 1.0;
                }
            }

            std::vector<Real> scratch;
            for (Index i = 0; i < assets; ++i)
            {
                History &h = histories[i];
                if (h.first == npos)
                {
                    out.add(QualityCode::NeverQuoted, Severity::Warning, i, market.dates.front(), "the asset has no quote at all");
                    continue;
                }
                if (h.last + 1 < days)
                {
                    out.add(QualityCode::SeriesEnds, Severity::Warning, i, market.dates[h.last + 1],
                            "the series stops before the end: no quote from that date on, the position is kept at its last price");
                }
                const Index gaps = h.last - h.first + 1 - h.quoted;
                if (gaps > 0)
                {
                    out.add(QualityCode::MissingQuotes, Severity::Warning, i, market.dates[h.firstGap],
                            "a day has no quote inside the life of the series", gaps);
                }
                if (h.returns.size() < 2)
                {
                    continue;
                }
                if (static_cast<Real>(h.zeros) > thresholds.stale_fraction * static_cast<Real>(h.returns.size()))
                {
                    out.add(QualityCode::StalePrices, Severity::Warning, i, market.dates[h.first],
                            "many closes are unchanged from the day before: volatility and correlations are understated");
                }
                scratch = h.returns;
                const Real center = median(scratch);
                for (Real &x : scratch)
                {
                    x = std::abs(x - center);
                }
                const Real scale = 1.4826 * median(scratch);
                if (!(scale > 0.0) || !std::isfinite(scale))
                {
                    continue;
                }
                for (Index k = 0; k < h.returns.size(); ++k)
                {
                    if (std::abs(h.returns[k] - center) > thresholds.extreme_return_deviations * scale)
                    {
                        out.add(QualityCode::ExtremeReturn, Severity::Warning, i, market.dates[h.days[k]],
                                "a return is extreme: check for a split missing from the events");
                    }
                }
            }
        }

    } // namespace

    bool QualityReport::usable() const noexcept
    {
        return std::none_of(issues.begin(), issues.end(), [](const QualityIssue &issue)
                            { return issue.severity == Severity::Error; });
    }

    Index QualityReport::count(QualityCode code) const noexcept
    {
        Index total = 0;
        for (const QualityIssue &issue : issues)
        {
            total += issue.code == code ? issue.count : 0;
        }
        return total;
    }

    std::string QualityReport::error_summary() const
    {
        std::string summary;
        for (const QualityIssue &issue : issues)
        {
            if (issue.severity == Severity::Error)
            {
                summary += summary.empty() ? "" : "\n";
                summary += issue.message;
            }
        }
        return summary;
    }

    QualityReport check_market_structure(const MarketData &market)
    {
        Collector out;
        if (!checkShapes(market, out))
        {
            return out.report();
        }
        const bool datesOk = checkDates(market, out);
        checkRates(market.cash_rate, market.dates, "the cash rate is missing or infinite", out);
        checkRates(market.debit_rate, market.dates, "the debit rate is missing or infinite", out);
        checkRates(market.borrow_rate, market.dates, "the borrow rate is missing or infinite", out);
        if (datesOk)
        {
            checkEvents(market, out);
        }
        return out.report();
    }

    QualityReport check_market_data(const MarketData &market, const QualityThresholds &thresholds)
    {
        QualityReport report = check_market_structure(market);
        if (!report.usable())
        {
            return report;
        }
        Collector bars;
        checkBars(market, thresholds, bars);
        const QualityReport barReport = bars.report();
        report.issues.insert(report.issues.end(), barReport.issues.begin(), barReport.issues.end());
        if (report.usable())
        {
            Collector histories;
            checkHistories(market, thresholds, histories);
            const QualityReport more = histories.report();
            report.issues.insert(report.issues.end(), more.issues.begin(), more.issues.end());
        }
        return report;
    }

} // namespace quantBacktest
