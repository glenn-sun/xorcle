# Xorcle

Xorcle is a SAT solver for XNF formulas, where clauses are disjunctions of linear equations modulo 2.

This repository contains:

- the C++ solver
- Python tooling for benchmarking, validation, and format conversion under `tools/`

For the Python scripts, see [tools/README.md](/Users/glennsun/Documents/Projects/parity-sat-cpp/tools/README.md).

**Note: Parts of this repository are written with OpenAI Codex. All code has been manually reviewed and tested.**

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

To also write a proof in LRUP(⊕) format:

```bash
bin/xorcle <input> <proof_path>
```

For the full command-line interface:

```bash
bin/xorcle --help
```

## File formats

### Extended DIMACS/XNF
Xorcle reads an extended DIMACS/XNF-style format, mirroring [2-Xornado](https://github.com/j-danner/2-Xornado). The file must begin with: `p cnf <num_vars> <num_clauses>` or `p xnf <num_vars> <num_clauses>`. Comment lines begin with `c`.

Each clause line must end with a trailing `0`. Plain Boolean clauses look like ordinary DIMACS:

```text
1 -3 7 0
```

Linear clauses are written with `+`-joined variable lists. For example, 

```text
-1+2+5 4+9 0
```

refers to the linear clause $\lnot (x_1 \oplus x_2 \oplus x_5) \lor (x_4 \oplus x_9)$.

### DRUP(⊕)

This is a custom proof format mirroring the above format for clauses. Input clauses get implicit IDs counting up from 1. Learned clauses in the proof are written `<new id> <clause> 0 <reason ids> 0`. The provided script `tools/check_drup_parity.py" script can verify DRUP(⊕) proofs.

## Tests and benchmark inputs

Benchmarks expect inputs under `tests/`, but they are not provided in the repository. Before running benchmarks, please assemble the following.

- `tests/generated`: Generate using `python3 tools/generate_xnf_tests.py`. 
- `tests/ascon/`: Download from [Ascon-128 benchmark set](https://github.com/j-danner/2xnf_sat_solving/tree/main/benchmark/ascon)
- `tests/sat-comp/`: Download from [SAT Competition 2025 CNF benchmark database](https://benchmark-database.de/?track=main_2025&context=cnf)

The benchmark tools assume those directories exist if you want to run the corresponding benchmark families.

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

The codebase includes Doxygen-style comments and a `Doxyfile`.

To build the HTML documentation locally:

```bash
doxygen Doxyfile
```

The generated output will be written under the Doxygen output directories configured by that file.
