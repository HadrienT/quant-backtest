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

**Status: accounting engine.** The event-driven engine is written and tested:
a book in shares and cash, dividends and splits applied from raw prices, a
declared signal-to-execution timeline, an order journal, proportional and
spread costs, data quality controls. Metrics, portfolio construction,
validation, the hedging backtest and the risk-model backtest are not written
yet, and the Python wheel does not expose the engine yet. The work packages
are listed in [`blueprint/README.md`](blueprint/README.md).

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

Requirements: CMake 3.25 or later, Ninja, a C++20 compiler, and a
[vcpkg](https://github.com/microsoft/vcpkg) clone pointed to by `VCPKG_ROOT`.
Dependencies are installed from [`vcpkg.json`](vcpkg.json) at configure time;
`quantModeling` is fetched at the commit pinned in
[`cmake/QuantModeling.cmake`](cmake/QuantModeling.cmake).

```bash
scripts/make.sh          # configure, build, run the tests
scripts/make_asan.sh     # same, under ASan and UBSan
scripts/build_wheel.sh   # Python wheel in dist/
scripts/bench.sh         # benchmark of the accounting engine
```

```python
import quantbacktest
quantbacktest.build_info()
# {'version': '0.1.0', 'git_sha': '...', 'quantmodeling_sha': '...'}
```

## License

MIT, see [`LICENSE`](LICENSE).
