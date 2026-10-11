#include <pybind11/pybind11.h>

#include "quantBacktest/core/build_info.hpp"

#include <string>

namespace py = pybind11;

// The interface carries plain data only (docs/contract.md, section 3): numpy
// arrays, numbers, strings, dictionaries. No C++ object is exposed.
PYBIND11_MODULE(quantbacktest, m)
{
    m.doc() = "quantBacktest: backtesting engine (strategy, hedging, risk models)";

    const quantBacktest::BuildInfo &info = quantBacktest::build_info();
    m.attr("__version__") = std::string(info.version);

    m.def(
        "build_info",
        []()
        {
            const quantBacktest::BuildInfo &b = quantBacktest::build_info();
            py::dict out;
            out["version"] = std::string(b.version);
            out["git_sha"] = std::string(b.git_sha);
            out["quantmodeling_sha"] = std::string(b.quantmodeling_sha);
            return out;
        },
        "What to record to replay a run: the version of the engine, the commit it was\n"
        "built from, and the commit of quantModeling the hedging module prices with.");
}
