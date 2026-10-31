# quant-backtest — notes for Claude Code

Backtest engine in **C++20**, exposed to Python by a pybind11 wheel `quantbacktest`. It covers the
four meanings of the word on a desk: backtest of a strategy, of a model and its hedge, of a market
risk model, of an exposure model.

**This repository contains no website, no database access and no pricing code.** It computes: it
receives arrays and returns arrays.

## Repository status

**All work packages of this repository are delivered: B0, B2, B3, B4, B5, B6 and B7.** B1 is
delivered in `data-ingest`; only B8 remains, in `quant-modeling` (issue #166 of that repository).
Modules present: `core/`, `data/`, `quality/`, `costs/`, `engine/`, `metrics/`, `inference/`,
`construction/`, `validation/`, `risk_models/`, `hedging/`, and the factor regression of
`attribution/`. The `quantbacktest` wheel exposes:

- `build_info()`, `fingerprint()`, `total_returns()`, `period_rates()`;
- `run_allocation()`, the backtest of a strategy with its comparison points, its gross run and its
  cost breakdown;
- `run_allocation_grid()`, one backtest per configuration, which returns the table expected by
  validation;
- `performance_report()`, `factor_regression()`, `sharpe_difference()`, and two default rules,
  `optimal_block_length()` and `newey_west_lag()`;
- `walk_forward()`, `combinatorial_purged_cv()`, `deflated_sharpe()`, `backtest_overfitting()`, on a
  table of returns with one column per configuration tried;
- `hedging_backtest()`, `fit_minimum_variance_delta()`;
- `var_backtest()`, `pla_test()`, `desk_backtest()`.

Their shape is in §5 of [`docs/contract.md`](docs/contract.md). A strategy is C++: the wheel does not
accept weights computed on the Python side, which would bypass point-in-time.

Available optimization constraints: per-asset bounds and budget, nothing else (ADR-010).

The hedging backtest runs in a **simulated world** only (Black-Scholes or Heston); the version on
historical surfaces awaits data (issue #15). It works around a defect of the `quantModeling` Heston
pricer far from the strike (ADR-011): to be removed when `quant-modeling#168` is fixed.

The accounting engine can sell short (`allow_short`, with a provided borrow rate), round to whole
lots (`whole_shares`) and charge a square-root market impact. What it does **not** do yet (issue #9):
the delisting return of an asset that stops trading (the position stays at its last price, flagged in
the quality report), for lack of data.

To know before using the estimated spread cost: on liquid assets the Corwin-Schultz and Abdi-Ranaldo
estimators overestimate the spread by an order of magnitude (§5.1 of the companion).

The repository was created on October 11, 2026 from work package 24 of the `quant-modeling`
blueprint, when the maintainer decided to write the engine in C++ and move it out of the pricing
repository. The old `/backtest` page of `quant-modeling` was hidden the same day; its Python code is
**not** reused, the engine is rewritten from scratch. Non-blocking leftovers of the work packages are
in the open issues. Work package B1 was delivered in `data-ingest` on October 11, 2026 (issue #14 of
that repository, closed: it describes the tables and their conventions). Update this section with
each delivered work package.

## Where to start

| File | Role |
|---|---|
| [`docs/companion.md`](docs/companion.md) | Reading companion **and** specification: desk practice (§1 to §13), the fifteen findings on the old page (§2), the architecture (§14), the work packages (§15), the property tests (§16), the state of the data (§17), the bibliography (§19) |
| [`docs/contract.md`](docs/contract.md) | Boundary with `quant-modeling`: who owns what, shape of the interface, version pinning |
| [`blueprint/README.md`](blueprint/README.md) | The work packages and their dependency graph: consult it before starting a work package |
| [`blueprint/decisions.md`](blueprint/decisions.md) | Architecture decisions, with the alternatives rejected |

## Relationship with the other repositories

| Repository | Role relative to this one |
|---|---|
| `~/quant-modeling` | **C++ dependency**: the `quantModeling` library (pricers, greeks, calibrations), pulled at a pinned commit, for the `hedging/` module only. **Consumer**: its API imports the `quantbacktest` wheel. It holds the FastAPI router, the database reads, the audit event, the replay and the `/backtest` page |
| `~/data-ingest` | Market data ingestion into Postgres. Work package B1 (raw prices, events, historical membership, factors) is done there |
| `~/quant-platform` | Kafka, audit database, observability. This repository emits nothing there: the `quant-modeling` API does |

Boundary rules:

- **Never edit a neighboring repository from a session opened here.** A change touching two
  repositories means one issue in each, referencing each other, and two coordinated PRs.
- **The dependency goes one way only.** This repository knows `quantModeling`; nothing in
  `quant-modeling/include` or `src` may know this repository.
- **The contract** is canonical here ([`docs/contract.md`](docs/contract.md)); changing it is an ADR.

## Non-negotiable architecture principles

1. **The engine reads no database, no file, no network.** SQL is in `quant-modeling`, ingestion in
   `data-ingest`. No Yahoo Finance, anywhere.
2. **Point-in-time by construction**: at date $t$, the engine gives the strategy only what was known
   at $t$. The "no future" test of §16 applies to any new data path.
3. **Never interpolation or fallback.** Missing data is a non-tradable day or an explicit error.
4. **The book is in shares and cash**, not in weights. Each order leaves a line in the journal.
5. **The strategy is separate from the engine**: the engine does not branch on any strategy, just as
   a `quantModeling` engine does not branch on any product.
6. **Only `hedging/` includes `quantModeling`.** The other modules build and test without it.
7. **The Python interface carries only simple data** (numpy arrays, numbers, strings, dictionaries).
   No object of the `quantmodeling` wheel crosses it.
8. **Determinism**: same inputs and same seed, same outputs bit for bit, whatever the number of
   threads.
9. **Every figure returned carries its convention and its uncertainty.** A metric that cannot be
   bounded is not returned.
10. **Stay close to the sources**: names, formulas and sign conventions are those of the references of
    §19; the reference is cited in the function's comment. The regulatory thresholds of §12 are
    rechecked against the primary text before being coded.

## Commands

`VCPKG_ROOT` must point to a vcpkg clone; dependencies are installed by the
[`vcpkg.json`](vcpkg.json) manifest at configure time.

| Command | Effect |
|---|---|
| `scripts/make.sh` | Configures (preset `default`), builds into `build/`, runs all tests |
| `scripts/make_asan.sh` | Same with ASan and UBSan, into `build-asan/` |
| `cmake --preset default && cmake --build build` | Build without running the tests |
| `ctest --test-dir build --output-on-failure` | All tests |
| `ctest --test-dir build -LE hedging` | Tests that do not need `quantModeling` |
| `ctest --test-dir build -R BuildInfo` | Tests whose name contains a pattern |
| `cmake --preset no-hedging && cmake --build build-no-hedging` | Build without `quantModeling` (principle 6): the short loop for everything except `hedging/` |
| `scripts/bench.sh` | Builds (preset `bench`, into `build-bench/`) and runs the benchmark; arguments go to google-benchmark, for example `--benchmark_filter=Monthly` |
| `scripts/build_wheel.sh` | `quantbacktest` wheel in `dist/` |
| `pytest` | Python tests, on the wheel installed in the current environment (`pip install dist/*.whl pytest`) |
| `pre-commit run --all-files` | `clang-format` 18 and `black` |

CMake targets: `quantBacktest` (everything except hedging, does not link `quantModeling`),
`quantBacktest_hedging` (the only one that links it), `quantBacktest_tests`,
`quantBacktest_hedging_tests` and `quantBacktest_bench`. A new source file or test is added by hand to
its target's list. Tests build their data with
[`tests/support/synthetic_market.hpp`](tests/support/synthetic_market.hpp). All random draws in the
library go through `core/random.hpp` (ADR-009); tests may use `<random>`.

**Bumping the `quantModeling` version**: change `QB_QUANTMODELING_SHA` in
`cmake/QuantModeling.cmake` (a commit SHA, never a branch), in a separate PR.

## Tracking work: GitHub Issues

`gh` is authenticated (account `HadrienT`).

- **At the start of a session**: `gh issue list --state open`.
- **Issue handled** → `gh issue close <n> --comment "done in <sha>"`.
- **Never** a markdown handoff file between sessions. A task that outlives the session is an issue.
- Design lives in `docs/` and `blueprint/`; the issue points to it, it does not replace it.

## Conventions

- **Language: English by default.** Everything you produce is in English: code, identifiers, comments, commit messages, PRs, issues, documentation and interface text. The only exception is the conversation: reply in the language the maintainer writes in, so in French when they write in French. `docs/` and `blueprint/` are historically in French: new content there is in English too.
- **Commits: go through a branch, never directly on `main`** (the initial documentation commit is the
  only exception). End messages with the session's `Co-Authored-By` line.
- **C++**: `clang-format --style=file`, **version 18**; the `.clang-format` is that of
  `quant-modeling`. **Python**: `black`. The pre-commit hook must be installed on each clone:
  `pre-commit install -t pre-commit`.
- **Memory ownership**: prefer `unique_ptr` and non-owning references.
- **Every module comes with its test**, preferably a *property* test (conservation, no future,
  invariance to splits, empirical size of a statistical test) rather than a numeric equality. The list
  is in §16 of the companion.
- **Declarative dependency management**: everything goes through `vcpkg.json` and the CMake presets; a
  fresh clone builds with no manual step.
- **No new product.** This project adds no payoff to the `quant-modeling` catalogue.
