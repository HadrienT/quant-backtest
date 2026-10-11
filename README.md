# quant-backtest

A backtesting engine in C++20, built the way a quant desk does it: point-in-time
data, a book kept in shares and cash, explicit transaction costs, and an
uncertainty on every figure it reports.

It covers the four things a desk calls "backtest":

| What is tested | What it answers |
|---|---|
| A **strategy** | Net of costs, against equal weight and a total-return benchmark: is the Sharpe ratio distinguishable from zero, and from luck after N trials? |
| A **model and its hedge** | If I delta-hedge with this model, what is left in my P&L? |
| A **market risk model** (VaR, ES) | Do realised losses match the announced ones, in number and in clustering? |
| An **exposure model** | Do realised values fall where the simulated distribution said? |

**Status: specification only.** There is no code yet. The design, the reading
companion and the work packages are written; the first work package (B0, the
build skeleton) has not started.

## Where it sits

This repository is the computation engine and nothing else. It is one of four
sibling repositories:

| Repository | Role |
|---|---|
| **quant-backtest** (here) | The engine: C++20 library, GoogleTest suite, pybind11 wheel `quantbacktest` |
| [quant-modeling](https://github.com/HadrienT/quant-modeling) | The derivatives pricing library `quantModeling` that the hedging backtest prices and calibrates with, and the website: FastAPI route, database reads, audit trail, the `/backtest` page |
| [data-ingest](https://github.com/HadrienT/data-ingest) | Market data ingestion into Postgres |
| [quant-platform](https://github.com/HadrienT/quant-platform) | Kafka, audit database, observability |

The engine never opens a database or a file: it receives arrays and returns
arrays. The boundary with `quant-modeling` is written down in
[`docs/contract.md`](docs/contract.md).

## Documentation

| File | What it is |
|---|---|
| [`docs/companion.md`](docs/companion.md) | Reading companion and specification (in French): what a desk means by backtesting, with formulas and references, what the previous page got wrong, the architecture, the work packages, the property tests |
| [`docs/contract.md`](docs/contract.md) | Who owns what between this repository and `quant-modeling` |
| [`blueprint/README.md`](blueprint/README.md) | Work packages and their dependency graph |
| [`blueprint/decisions.md`](blueprint/decisions.md) | Architecture decisions, with the alternatives rejected |
| [`CLAUDE.md`](CLAUDE.md) | Working notes for Claude Code sessions |

## Build

Not available yet: the build files arrive with work package B0. The intended
toolchain is the one `quant-modeling` uses (CMake presets, Ninja, vcpkg,
GoogleTest, clang-format 18).

## License

MIT, see [`LICENSE`](LICENSE).
