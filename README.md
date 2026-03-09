# Xorcle

Xorcle is a SAT solver for formulas with XOR structure.

This repository contains:

- the C++ solver
- example and generated test inputs under `tests/`
- Python tooling for benchmarking, validation, and format conversion under `tools/`

For the Python scripts, see [tools/README.md](/Users/glennsun/Documents/Projects/parity-sat-cpp/tools/README.md).

## Building

From the repository root:

```bash
make
```

This builds the default release binary:

```text
bin/xorcle
```

Other useful targets:

- `make test`
  - builds Xorcle and runs a small smoke test
- `make debug`
  - builds a debug-oriented binary at `bin/xorcle-debug`
- `make profile`
  - builds a profiling-oriented binary at `bin/xorcle-profile`
- `make clean`
  - removes build outputs

## Running

Basic usage:

```bash
bin/xorcle <input>
```

To also write a proof:

```bash
bin/xorcle <input> <proof_path>
```

For the full command-line interface:

```bash
bin/xorcle --help
```

## Input files

Xorcle reads DIMACS-like inputs with support for XOR structure in addition to plain CNF.

The repository includes example inputs under:

- `tests/generated/`
- `tests/ascon/`
- `tests/sat-comp/`

## Output

Xorcle prints DIMACS-style result output:

- comment/stat lines beginning with `c `
- a result line:
  - `s SATISFIABLE`
  - `s UNSATISFIABLE`
- for SAT, assignment lines beginning with `v `

## Repository layout

If you are looking for the main solver code:

- `src/Main.cpp`
  - solver CLI
- `src/search/`
  - solver loop, trail, propagation, compaction, restart handling
- `src/heuristics/`
  - decision procedures and clause selectors
- `src/algebra/`
  - GF(2) equations, row spaces, and related algebra
- `src/io/`
  - parsing and proof logging
- `src/util/`
  - stats, deterministic randomness, and small shared utilities

## Documentation

The codebase includes Doxygen-style comments and a `Doxyfile` if you want generated API documentation.
