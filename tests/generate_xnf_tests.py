#!/usr/bin/env python3
"""
This file was written with the assistance of generative AI.

generate_xnf_tests.py

Generate CNF files in the "plus-joined, no parentheses" DIMACS extension.

Sampling rules implemented:
 - Dense: each variable is independently included with probability 1/2.
          If all-zero (empty equation) occurs, we resample until non-empty.
 - Sparse (max size s): uniformly sample a non-empty subset of size <= s.
                        Achieved by picking k with weight C(n,k) and then
                        sampling a uniform k-subset.
 - Dense k-XNF: each clause is a disjunction of k random dense linear equations.
                The clause count m is chosen so that the expected number of
                satisfying assignments is 1/2.

Output format:
 - tokens separated by spaces, clause terminated by "0"
 - a leading '-' on a token indicates RHS = 0 (otherwise RHS = 1)
 - e.g. "1+2 -3 0" or "-1+3+4 2 0"
"""

from math import comb, floor, log, log1p
import random
from typing import List, Optional

def _format_equation(vars_: List[int], rhs_zero: bool) -> str:
    """Format one equation token: vars_ is a list of 1-based indices (non-empty)."""
    if not vars_:
        raise ValueError("Equation must contain at least one variable")
    body = "+".join(str(v) for v in vars_)
    return ("-" if rhs_zero else "") + body

def _sample_dense_subset_nonempty(n: int) -> List[int]:
    """Sample each bit independently with p=1/2. Resample if empty."""
    while True:
        # include variable i with probability 1/2
        picked = [i+1 for i in range(n) if random.getrandbits(1)]
        if picked:
            return picked
        # else repeat (probability of repeating is 2^-n, tiny except for small n)

def _sample_uniform_subset_upto_s(n: int, s: int) -> List[int]:
    """
    Uniformly sample a non-empty subset of {1..n} whose size is <= s.
    Procedure:
      - compute counts C(n,1)+...+C(n,s)
      - pick an integer r in [0, total-1]
      - find k such that cumulative C(n,1..k) > r
      - sample a uniform k-subset via random.sample
    """
    if s < 1:
        raise ValueError("s must be >= 1")
    s = min(s, n)
    # compute cumulative counts
    counts = [comb(n, k) for k in range(1, s+1)]
    total = sum(counts)
    r = random.randrange(total)
    cum = 0
    for idx, cnt in enumerate(counts):  # idx corresponds to k = idx+1
        if r < cum + cnt:
            k = idx + 1
            break
        cum += cnt
    else:
        # should not happen
        k = s
    return random.sample(range(1, n+1), k=k)

def _expected_kxnf_clause_count(n: int, k: int) -> int:
    """
    Compute the clause count m for dense k-XNF such that
      E[#SAT] = 2^n * (1 - 2^-k)^m = 1/2.
    Thus:
      m = (n + 1) * ln 2 / -ln(1 - 2^-k).
    """
    if n < 1 or k < 1:
        raise ValueError("n and k must be >= 1")
    p_unsat = 2.0 ** (-k)
    denom = -log1p(-p_unsat)
    m = (n + 1) * log(2.0) / denom
    return int(round(m))

def generate_random_system(m: int,
                           n: int,
                           filepath: str,
                           *,
                           sparse_max_size: Optional[int] = None,
                           seed: Optional[int] = None) -> None:
    """
    Generate a CNF file representing a system of m linear equations in n variables.

    Parameters:
      m: number of clauses (equations)
      n: number of variables (1..n)
      filepath: output filename
      sparse_max_size: if None -> dense Bernoulli(1/2) per variable;
                       otherwise sample uniformly among non-empty subsets of size <= sparse_max_size
      seed: optional RNG seed for reproducibility
    """
    if seed is not None:
        random.seed(seed)

    if n < 1 or m < 0:
        raise ValueError("n must be >= 1 and m must be >= 0")

    lines = [f"p cnf {n} {m}"]

    for _ in range(m):
        if sparse_max_size is None:
            vars_ = _sample_dense_subset_nonempty(n)
        else:
            vars_ = _sample_uniform_subset_upto_s(n, sparse_max_size)
        rhs_zero = random.choice([False, True])  # uniformly pick RHS 0/1
        token = _format_equation(vars_, rhs_zero)
        lines.append(f"{token} 0")

    with open(filepath, "w") as f:
        f.write("\n".join(lines) + "\n")

def generate_dense_kxnf(n: int,
                        k: int,
                        filepath: str,
                        *,
                        seed: Optional[int] = None) -> int:
    """
    Generate a CNF file representing a dense random k-XNF on n variables.

    Each clause is a disjunction of k random dense linear equations.
    The number of clauses m is chosen so E[#SAT] = 1/2.
    Returns the computed m.
    """
    if seed is not None:
        random.seed(seed)

    if n < 1 or k < 1:
        raise ValueError("n and k must be >= 1")

    m = _expected_kxnf_clause_count(n, k)
    lines = [f"p cnf {n} {m}"]

    for _ in range(m):
        tokens = []
        for _ in range(k):
            vars_ = _sample_dense_subset_nonempty(n)
            rhs_zero = random.choice([False, True])
            tokens.append(_format_equation(vars_, rhs_zero))
        lines.append(f"{' '.join(tokens)} 0")

    with open(filepath, "w") as f:
        f.write("\n".join(lines) + "\n")
    return m

def generate_mixed_system(n: int,
                          filepath: str,
                          *,
                          seed: Optional[int] = None) -> None:
    """
    Generate a CNF file containing:
      - floor(n/2) single-equation clauses
      - n clauses of length 2; each equation in these clauses is uniformly sampled
        among subsets with size <= floor(n/20)

    Parameters:
      n: number of variables
      filepath: output filename
      seed: optional RNG seed
    """
    if seed is not None:
        random.seed(seed)

    if n < 1:
        raise ValueError("n must be >= 1")

    m1 = n // 2
    m2 = n
    total = m1 + m2
    lines = [f"p cnf {n} {total}"]

    # first block: m1 clauses, either dense or sparse as requested
    for _ in range(m1):
        vars_ = _sample_dense_subset_nonempty(n)
        rhs_zero = random.choice([False, True])
        lines.append(f"{_format_equation(vars_, rhs_zero)} 0")

    # second block: m2 clauses of length 2, each equation size <= floor(n/20)
    for _ in range(m2):
        vars1 = _sample_dense_subset_nonempty(n)
        vars2 = _sample_dense_subset_nonempty(n)
        rhs1 = random.choice([False, True])
        rhs2 = random.choice([False, True])
        lines.append(f"{_format_equation(vars1, rhs1)} {_format_equation(vars2, rhs2)} 0")

    with open(filepath, "w") as f:
        f.write("\n".join(lines) + "\n")

# ---------------------
# Small CLI for testing
# ---------------------
if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser(description="Generate uniformly random DIMACS-plus CNF files.")
    sub = parser.add_subparsers(dest="cmd", required=True)

    p1 = sub.add_parser("random", help="Generate m random equations in n variables.")
    p1.add_argument("m", type=int)
    p1.add_argument("n", type=int)
    p1.add_argument("out", help="output file")
    p1.add_argument("--sparse_max_size", type=int, default=None,
                    help="If provided, sample each equation uniformly from subsets of size <= this.")
    p1.add_argument("--seed", type=int, default=None)

    p2 = sub.add_parser("mixed", help="Generate floor(n/2) eqs then n clauses of length 2.")
    p2.add_argument("n", type=int)
    p2.add_argument("out", help="output file")
    p2.add_argument("--seed", type=int, default=None)

    p3 = sub.add_parser("kxnf", help="Generate dense random k-XNF with m at the 50 percent expectation threshold.")
    p3.add_argument("n", type=int)
    p3.add_argument("k", type=int)
    p3.add_argument("out", help="output file")
    p3.add_argument("--seed", type=int, default=None)

    args = parser.parse_args()
    if args.cmd == "random":
        generate_random_system(args.m, args.n, args.out,
                               sparse_max_size=args.sparse_max_size, seed=args.seed)
        print(f"Wrote random system m={args.m}, n={args.n} to {args.out}")
    elif args.cmd == "mixed":
        generate_mixed_system(args.n, args.out,
                              seed=args.seed)
        print(f"Wrote mixed system for n={args.n} to {args.out}")
    else:
        m = generate_dense_kxnf(args.n, args.k, args.out, seed=args.seed)
        print(f"Wrote dense k-XNF m={m}, n={args.n}, k={args.k} to {args.out}")
