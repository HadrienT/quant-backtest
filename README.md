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

**Status: accounting engine, metrics and inference.** Written and tested:

- the event-driven engine: a book in shares and cash, dividends and splits
  applied from raw prices, a declared signal-to-execution timeline, an order
  journal, proportional and spread costs, data quality controls;
- performance metrics, each returned with its interval: the standard error of
  the Sharpe ratio, the probabilistic Sharpe ratio, the stationary bootstrap;
- the factor regression, with an annualised alpha and Newey-West errors;
- portfolio construction: equal weight, minimum variance, maximum Sharpe ratio
  and risk parity, with a Ledoit-Wolf covariance and bounds held inside the
  optimiser; a strategy is always returned with the equal-weight portfolio and
  buy-and-hold next to it, and a test of the difference between two Sharpe
  ratios;
- the backtest of a value-at-risk model: Kupiec and Christoffersen tests, and
  the Basel traffic-light zones.

Validation (walk-forward, deflated Sharpe ratio) and the hedging backtest are
not written yet. The work packages are listed in
[`blueprint/README.md`](blueprint/README.md).

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
import numpy as np
import quantbacktest as qb

report = qb.performance_report(
    returns, cash_rate,              # one rate per period, the rate of the time
    bootstrap_replications=1000, mean_block_length=10.0, seed=1,
)
report["metrics"]["sharpe_ratio"]
# {'value': 0.81, 'standard_error': 0.58, 'lower': -0.33, 'upper': 1.95,
#  'confidence': 0.95, 'method': 'Lo (2002) standard error ...', 'convention': ...}

qb.factor_regression(excess, factors, newey_west_lags=5)["alpha"]
qb.var_backtest(pnl, value_at_risk, tail_probability=0.01)["traffic_light"]
qb.build_info()   # version, commit, commit of quantModeling
```

`qb.run_allocation(dates, close, method="minimum_variance", ...)` runs a
strategy on raw prices and returns the book day by day, with its comparison
points. The functions are listed in [`docs/contract.md`](docs/contract.md).

## License

MIT, see [`LICENSE`](LICENSE).
