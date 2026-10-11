#include "quantBacktest/costs/cost_model.hpp"

#include "quantBacktest/core/error.hpp"

#include <cmath>
#include <cstdio>
#include <limits>
#include <utility>

namespace quantBacktest
{

    void CostModel::validate(const MarketData &) const {}

    ProportionalCost::ProportionalCost(Real rate)
        : rate_(rate)
    {
        if (!(std::isfinite(rate) && rate >= 0.0))
        {
            throw Error("the proportional cost rate must be a non-negative number");
        }
    }

    Real ProportionalCost::cost(const MarketView &, const Fill &fill) const
    {
        return rate_ * std::abs(fill.quantity) * fill.price;
    }

    std::string ProportionalCost::describe() const
    {
        char buffer[96];
        std::snprintf(buffer, sizeof(buffer), "proportional cost of %g basis points of the amount traded", rate_ * 1e4);
        return buffer;
    }

    SpreadCost::SpreadCost(MatrixView spreads, std::string source)
        : spreads_(spreads), source_(std::move(source))
    {
        if (spreads.empty())
        {
            throw Error("the spread cost needs a table of spreads");
        }
    }

    void SpreadCost::validate(const MarketData &market) const
    {
        if (spreads_.rows() != market.dates.size() || spreads_.cols() != market.assets)
        {
            throw Error("the table of spreads must have one row per date and one column per asset");
        }
    }

    Real SpreadCost::cost(const MarketView &, const Fill &fill) const
    {
        const Real spread = spreads_(fill.day, fill.asset);
        if (spread < 0.0)
        {
            throw Error("a spread is negative");
        }
        return 0.5 * spread * std::abs(fill.quantity) * fill.price;
    }

    std::string SpreadCost::describe() const
    {
        return "half the bid-ask spread on the amount traded (" + source_ + ")";
    }

    CostStack::CostStack(std::vector<const CostModel *> models)
        : models_(std::move(models))
    {
        for (const CostModel *model : models_)
        {
            if (model == nullptr)
            {
                throw Error("a cost stack cannot hold a null model");
            }
        }
    }

    Real CostStack::cost(const MarketView &market, const Fill &fill) const
    {
        Real total = 0.0;
        for (const CostModel *model : models_)
        {
            total += model->cost(market, fill);
        }
        return total;
    }

    void CostStack::validate(const MarketData &market) const
    {
        for (const CostModel *model : models_)
        {
            model->validate(market);
        }
    }

    std::string CostStack::describe() const
    {
        std::string text;
        for (const CostModel *model : models_)
        {
            text += text.empty() ? "" : "; plus ";
            text += model->describe();
        }
        return text.empty() ? "no cost" : text;
    }

} // namespace quantBacktest
