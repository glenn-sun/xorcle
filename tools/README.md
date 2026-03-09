# Tools

This directory contains two kinds of Python code:

- **Direct-use scripts** you run from the command line.
- **Support modules** imported by those scripts.

The easiest way to understand the directory is by role, not alphabetically.

## Prerequisites

### Base requirements

Most scripts require only:

- Python 3
- the repository checkout itself

Many scripts assume they are run from the repo root, or at least with paths relative to the repo root.

### Plotting

The benchmark plotting scripts use `matplotlib`. In this repository, the expected way to get that is the project venv, so prefer:

```bash
venv/bin/python tools/<script>.py ...
```

Use the system `python3` only for tools that do not need plotting dependencies.

### Benchmarking external solvers

The benchmark scripts assume these binaries exist if you benchmark them:

- `bin/xorcle` for **Xorcle**
- `external/kissat`
- `external/cryptominisat5`
- `external/2xornado`

The paper-facing benchmark scripts are specialized and only use the solver set they are designed for.

### Test generation

`generate_xnf_tests.py` needs extra Python packages for graph/formula generation:

- `networkx`
- `cnfgen`

If those packages are missing, the script fails explicitly rather than partially working.

### Proof checking

`test_solver_correctness_matrix.py` may call the DRUP+ checker in this directory for UNSAT validation, so correctness-matrix runs expect the local proof-checking path to be available.

## How to think about the scripts

### 1. Benchmark entrypoints

These are the scripts you use to produce benchmark data or paper plots.

- [`run_benchmark.py`](/Users/glennsun/Documents/Projects/parity-sat-cpp/tools/run_benchmark.py)
  - Low-level benchmark runner.
  - Runs selected solvers on selected inputs and emits raw benchmark rows.
  - Useful when you want a generic benchmark job rather than one of the paper figures.

- [`benchmark_representative_cactus.py`](/Users/glennsun/Documents/Projects/parity-sat-cpp/tools/benchmark_representative_cactus.py)
  - Produces the all-solver cactus figure on the representative set.

- [`benchmark_satcomp_ascon_cactus.py`](/Users/glennsun/Documents/Projects/parity-sat-cpp/tools/benchmark_satcomp_ascon_cactus.py)
  - Produces the side-by-side all-solver cactus figure for SAT competition inputs and Ascon inputs.

- [`benchmark_tseitin_regression.py`](/Users/glennsun/Documents/Projects/parity-sat-cpp/tools/benchmark_tseitin_regression.py)
  - Produces the two-panel Tseitin regression figure.

- [`benchmark_xorcle_heuristics.py`](/Users/glennsun/Documents/Projects/parity-sat-cpp/tools/benchmark_xorcle_heuristics.py)
  - Produces the Xorcle-only heuristic comparison figure.

#### Shared benchmark behavior

The benchmark scripts are intentionally specialized, but they share the same execution model:

- benchmarked solver processes use a configurable memory cap
- results are cached in a shared execution cache under `benchmarks/cache/executions/`
- output directories are artifact folders for plots, manifests, and raw CSVs
- `--replot-only` uses local output artifacts and does not rerun solvers
- repeated solver reruns are no longer part of the benchmark model; each selected solver/config/file pair is run once

Representative-set benchmarks use deterministic sampling, so increasing the sample size reuses the earlier prefix instead of drawing a completely different set.

### 2. Validation and checking

- [`test_solver_correctness_matrix.py`](/Users/glennsun/Documents/Projects/parity-sat-cpp/tools/test_solver_correctness_matrix.py)
  - Regression harness for Xorcle.
  - Runs selected Xorcle configurations, checks SAT assignments, and checks UNSAT proofs.
  - This is the main solver-validation script, not a paper benchmark.

- [`check_drup_plus.py`](/Users/glennsun/Documents/Projects/parity-sat-cpp/tools/check_drup_plus.py)
  - Local DRUP(+) proof checker used by the repo’s proof-validation flow.

### 3. XNF generation and conversion

- [`generate_xnf_tests.py`](/Users/glennsun/Documents/Projects/parity-sat-cpp/tools/generate_xnf_tests.py)
  - Generates the repo’s synthetic benchmark families and writes manifests for them.

- [`xnf_convert.py`](/Users/glennsun/Documents/Projects/parity-sat-cpp/tools/xnf_convert.py)
  - Command-line wrapper for converting XNF into other solver-facing formats.
  - Use this when you want a one-off conversion from the shell.

#### Important format note

The Python XNF parser is strict. Clause lines must end with a trailing `0`, matching the stricter parser direction used elsewhere in the repo.

### 4. Support modules

These are not the main things to run directly. They exist to keep the entrypoint scripts smaller and more consistent.

- [`benchmark_core.py`](/Users/glennsun/Documents/Projects/parity-sat-cpp/tools/benchmark_core.py)
  - Shared benchmark execution, caching, row construction, summaries, and solver invocation logic.

- [`benchmark_orchestration.py`](/Users/glennsun/Documents/Projects/parity-sat-cpp/tools/benchmark_orchestration.py)
  - Shared benchmark metadata, representative-set sampling rules, solver labels, and progress formatting.

- [`benchmark_plot_style.py`](/Users/glennsun/Documents/Projects/parity-sat-cpp/tools/benchmark_plot_style.py)
  - Shared visual/layout constants for the benchmark figures.
  - This is the main place to edit figure width, spacing, font sizing, and related paper-plot defaults.

- [`plot_cactus.py`](/Users/glennsun/Documents/Projects/parity-sat-cpp/tools/plot_cactus.py)
  - Shared cactus plotting helper used by the benchmark entrypoints.

- [`tool_common.py`](/Users/glennsun/Documents/Projects/parity-sat-cpp/tools/tool_common.py)
  - Small repo/tool utility helpers.

- [`tool_process.py`](/Users/glennsun/Documents/Projects/parity-sat-cpp/tools/tool_process.py)
  - Shared subprocess launching, timeout handling, memory-cap handling, and solver-output parsing.

- [`xnf_lib.py`](/Users/glennsun/Documents/Projects/parity-sat-cpp/tools/xnf_lib.py)
  - Shared XNF parsing and conversion logic used by the XNF-facing scripts.

## Which script should I use?

- I want a paper figure:
  - use one of the `benchmark_*.py` entrypoints

- I want raw benchmark rows for an arbitrary input set:
  - use `run_benchmark.py`

- I want to validate current Xorcle behavior:
  - use `test_solver_correctness_matrix.py`

- I want to generate or regenerate synthetic test families:
  - use `generate_xnf_tests.py`

- I want to convert XNF to another format:
  - use `xnf_convert.py`

## Editing guidance

If you need to change how the tools look or behave, the main ownership boundaries are:

- benchmark execution/caching/parsing:
  - `benchmark_core.py`
- benchmark selection and labels:
  - `benchmark_orchestration.py`
- benchmark figure geometry and shared visual defaults:
  - `benchmark_plot_style.py`
- cactus rendering:
  - `plot_cactus.py`
- XNF parsing/conversion:
  - `xnf_lib.py`
- command-line surfaces:
  - the entrypoint scripts themselves

If a change is only about plot appearance, start in `benchmark_plot_style.py` before touching the benchmark entrypoints.
