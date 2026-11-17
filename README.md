# Xorcle

Xorcle is a SAT solver for XNF formulas, where clauses are disjunctions of
linear equations modulo 2. It implements CDCL(⊕) as described in *Extending CDCL
to disjunctions of parity equations* by Paul Beame and Glenn Sun. For details, see
[arXiv:2605.15002](https://arxiv.org/abs/2605.15002). When citing this work,
refer to the conference version at
[https://doi.org/10.4230/LIPIcs.SAT.2026.5](https://doi.org/10.4230/LIPIcs.SAT.2026.5). 

*This repository contains code written by generative AI. Authors retain full responsibility over code and text.*

## Building

Requires CMake ≥ 3.31, a C++23 compiler, and git. Builds are not portable across
machines. With `rustc` 1.73 or later on the `PATH`, the build also makes the proof
checker, `build/check_lrup_parity`.

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

You can also compile with `-DCMAKE_BUILD_TYPE=Debug` to ensure that more debug
assertions are checked. 

## Running

To run Xorcle, use the following command. Xorcle accepts CNF instances in standard
DIMACS format, CNF-XOR instances in DIMACS format extended with x-lines (as used
for [CryptoMiniSat](https://github.com/msoos/cryptominisat)), and XNF instances 
in an extended DIMACS/XNF format (see below for format description).

```bash
xorcle [options] <instance>
```

Run `xorcle --help` to print the full list of options.

## Testing

To verify that Xorcle is working properly, you can run one of the following commands. 

```bash
ctest --test-dir build        # Release

# Debug (takes longer, checks internal invariants)
cmake --preset dev && cmake --build --preset dev && ctest --preset dev
```

Some tests require Python 3.11+ and/or `rustc` 1.73+. These tests are not run if these 
are not available in the PATH.

## Output

Xorcle prints progress reports as it solves an instance. 
Configure these progress reports with `--report-cadence`and `--report-type`.

```text
c   time      confl  confl/s    props  clauses    avg lvl    avg trl    var rem
c   3.24        800    296.3    44836     3801       18.7      209.2        221
c   6.02       1600    263.4    82609     3858       17.8      183.3        197
```

More statistics will be available after solving has completed. Save them to a
file with `--stats-file` (this will also mute the progress reports).

```text
c File: instance.xnf
c Variables: 17

...

s SATISFIABLE
v 1 2 3 -4 -5 -6 -7 -8 9 10 11 -12 -13 -14 -15 16 -17 0
```

## File formats

### Extended DIMACS/XNF

Xorcle reads an extended DIMACS/XNF format, mirroring
[2-Xornado](https://github.com/j-danner/2xnf_sat_solving) and
[Xorricane](https://github.com/j-danner/Xorricane/). The file must begin
with `p cnf <num_vars> <num_clauses>` or `p xnf <num_vars> <num_clauses>`. Comment
lines begin with `c`.

Each clause line is one clause and must end with a trailing `0`. XOR expressions
in clauses are written with `+`-joined variable lists and are taken to be true
(= 1) by default. A leading `-` sign negates the expression (= 0). `+` is XOR,
so a variable named twice in one expression cancels. (Accordingly, any valid
DIMACS CNF is automatically a valid XNF file.) For example,

```text
-1+2+5 4+9 3 0
```

is the linear clause $\lnot (x_1 \oplus x_2 \oplus x_5) \lor (x_4 \oplus x_9) \lor x_3$.
The number of clause lines should match the header's count.

### LRUP(⊕)

Xorcle supports proof logging in LRUP(⊕). Enable proof output with `--proof=<path>`.
Note that proof logging may incur a small performance penalty.
The following describes the LRUP(⊕) text proof format.

Assign IDs to input clauses in the order they appear in the input file, starting from 1. 
An LRUP(⊕) proof file contains lines in the following format:

```text
<new id> <learned clause> 0 <reason id> <reason id> ... 0
```

- `<new id>` continues counting up from the last input clause.
- `<learned clause>` is a learned clause made of possibly negated `+`-joined variable lists,
  similar to the input DIMACS/XNF format. 
- `<reason id> <reason id> ...` is a list of clause IDs such that unit propagation on the mentioned 
clauses in the specified order results in a contradiction, starting from the negation of the learned
clause. The last reason must be falsified completely, and each prior reason must reduce to a unit at the time that it is used.

A proof is complete when learning the empty clause, with a proof line 
`<new id> 0 <reason id> <reason id> ... 0`.
Lines of the form `<reason id> <reason id> ... 0` (with a single 0) are also permitted, 
and indicate that the listed reasons will not be used again and can be deleted for memory
efficiency. 

> By default, Xorcle outputs a proof in a binary format. Switch to the above text format using
> `--proof-format=text`. The binary format is similar to binary DRAT and LRAT, where a learned 
> line starts with the byte `a` (0x61) and a deletion line starts with the byte `d` (0x64). 
> Otherwise, the format is the same, with each number expressed in binary and without spaces.
> Specifically, numbers appear in little-endian binary, cut into 7-bit groups, and with the high
> bit of every byte set, except the last byte, where the high bit is unset. 
> Each XOR expression (a possibly negated `+`-joined variable list) involving `k` variables
> is represented with `k+1` numbers (a sparse representation). First, there is a header number `2k + c`, 
> where `c` is 1 when the expression starts with `-` and 0 otherwise. Then, if `v1 < ... < vk` are 
> the variable *numbers*, we write down the consecutive gaps `v1`, `v2 - v1`, ..., `vk - v(k-1)`. 
> Where $n$ is the number of variables, this representation limits the number of bits per equation to 
> $8n + o(n)$, compared to $8n \lceil \log_2(n) / 7 \rceil + o(n)$ for writing plain variable numbers
> instead of gaps. 
> 
> As an example, the line `5 -3+17+200 0 2 4 0` is the following bytes.
> 
> ```text
> 61 05 07 03 0e b7 01 00 02 04 00
> ```

Use `build/check_lrup_parity`, built from `tools/check_lrup_parity.rs`, to check a proof. It auto-detects
whether a given proof is binary or text.

```bash
build/xorcle --proof=out.lrup instance.xnf
build/check_lrup_parity instance.xnf out.lrup
```

## Tools

The `tools/` directory has the proof checker in Rust and the scripts the tests use.

| File | Purpose |
|---|---|
| `check_lrup_parity.rs` | LRUP(⊕) proof checker, built as `build/check_lrup_parity` |
| `xnf_lib.py` | XNF parsing, writing, and other tools |
| `check_cli.sh` | Test that the CLI is working correctly |
| `run_with_proof_check.sh` | Solve an UNSAT instance with `--proof` and check the proof |

## Repository layout

- `src/algebra/` — equations over $\mathbb{F}_2$ and row-echelon form matrices, 
  with `dense/` and `sparse/` representations
- `src/core/` — core algorithms, including the CDCL(⊕) loop, unit propagation,
  and clause learning
- `src/heuristics/` — decisions, clause selection, restarts, clause deletion,
  compaction, the implication graph of the width-2 clauses, bounded
  projection (BVE), etc.
- `src/io/` — DIMACS/XNF parsing and LRUP(⊕) proof output
- `src/util/` — shared utilities and statistics
- `src/cli/` — option table and executable
- `tools/` — the LRUP(⊕) proof checker and the scripts the tests use
- `tests/` — unit tests and proof checks

## Documentation

The source carries Doxygen comments. To build the HTML reference:

```bash
doxygen docs/Doxyfile
```

Output lands in `docs/html/`. Open `docs/html/index.html` afterwards to view
documentation.

## For reviewers

The evaluation for Xorcle v0.2, corresponding to 
[arXiv:2605.15002v2](https://arxiv.org/abs/2605.15002v2), is in the 
[`paper-benchmarks`](https://github.com/glenn-sun/xorcle/tree/paper-benchmarks) branch.

The evaluation for Xorcle v0.1, corresponding to 
[arXiv:2605.15002v1](https://arxiv.org/abs/2605.15002v1), is in the `tools/` directory of the 
[`v0.1 tagged commit`](https://github.com/glenn-sun/xorcle/tree/v0.1).