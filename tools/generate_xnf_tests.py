#!/usr/bin/env python3
"""Generate benchmark families for parity-sat-cpp."""

from __future__ import annotations

import argparse
import random
from dataclasses import dataclass
from math import log, log1p
from pathlib import Path
from typing import Iterable, List, Sequence

try:
    import networkx as nx
    from cnfgen import PebblingFormula, PigeonholePrinciple, TseitinFormula
    from cnfgen.graphs import Graph, dag_pyramid
except ModuleNotFoundError:
    nx = None  # type: ignore[assignment]
    PebblingFormula = None  # type: ignore[assignment]
    PigeonholePrinciple = None  # type: ignore[assignment]
    TseitinFormula = None  # type: ignore[assignment]
    Graph = None  # type: ignore[assignment]
    dag_pyramid = None  # type: ignore[assignment]


@dataclass(frozen=True)
class FamilySpec:
    name: str
    input_format: str
    kind: str  # "random" or "deterministic"
    x_label: str
    x_values: Sequence[int]


FAMILIES: dict[str, FamilySpec] = {
    # "random_2xnf_n": FamilySpec("random_2xnf_n", "xnf", "random", "n", [17, 18, 19, 20, 21, 22, 23, 24, 25]),
    "random_3xnf_n": FamilySpec("random_3xnf_n", "xnf", "random", "n", [12, 13, 14, 15, 16, 17, 18, 19, 20]),
    "random_6xnf_n": FamilySpec("random_6xnf_n", "xnf", "random", "n", [8, 9, 10, 11, 12, 13, 14, 15, 16]),
    # "random_kxnf_15": FamilySpec("random_kxnf_15", "xnf", "random", "k", [2, 3, 4, 5, 6, 7]),
    # "restricted_2xnf_n": FamilySpec("restricted_2xnf_n", "xnf", "random", "n", [30, 32, 34, 36, 38, 40, 42, 44, 46, 48, 50]),
    "restricted_3xnf_n": FamilySpec("restricted_3xnf_n", "xnf", "random", "n", [20, 22, 24, 26, 28, 30, 32, 34, 36]),
    "restricted_6xnf_n": FamilySpec("restricted_6xnf_n", "xnf", "random", "n", [16, 18, 20, 22, 24, 26, 28, 30]),
    # "restricted_kxnf_25": FamilySpec("restricted_kxnf_25", "xnf", "random", "k", [2, 3, 4, 5, 6, 7]),
    # "pebbling_2lift_h": FamilySpec("pebbling_2lift_h", "xnf", "deterministic", "h", [100, 150, 200, 250, 300, 350, 400]),
    "pebbling_3lift_h": FamilySpec("pebbling_3lift_h", "xnf", "deterministic", "h", [50, 75, 100, 125, 150, 175, 200, 225, 250]),
    "pebbling_10lift_h": FamilySpec("pebbling_10lift_h", "xnf", "deterministic", "h", [40, 60, 80, 100, 120, 140, 160, 200]),
    # "pebbling_klift_100": FamilySpec("pebbling_klift_100", "xnf", "deterministic", "k", [2, 3, 4, 5, 6, 7]),
    "bijphp_n_n+1": FamilySpec("bijphp_n_n+1", "cnf", "deterministic", "n", [4, 5, 6, 7, 8]),
    "tseitin_k_20": FamilySpec("tseitin_k_20", "cnf", "random", "k", [3, 4, 5, 6, 7, 8, 9]),
    "tseitin_5_n": FamilySpec("tseitin_5_n", "cnf", "random", "n", [10, 20, 40, 80, 160, 320]),
}


def _format_equation(vars_: Sequence[int], rhs_zero: bool) -> str:
    if not vars_:
        raise ValueError("Equation must contain at least one variable")
    body = "+".join(str(v) for v in vars_)
    return ("-" if rhs_zero else "") + body


def _sample_dense_subset_nonempty(n: int, rng: random.Random) -> List[int]:
    while True:
        picked = [i + 1 for i in range(n) if rng.getrandbits(1)]
        if picked:
            return picked


def _expected_kxnf_clause_count(n: int, k: int, fixed_equations: int = 0) -> int:
    if n < 1 or k < 1:
        raise ValueError("n and k must be >= 1")
    if fixed_equations < 0 or fixed_equations > n:
        raise ValueError("fixed_equations must be in [0, n]")
    p_unsat = 2.0 ** (-k)
    denom = -log1p(-p_unsat)
    m = (n - fixed_equations + 1) * log(2.0) / denom
    return int(round(m))


def _write_xnf(path: Path, nvars: int, clauses: Iterable[Sequence[str]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    clause_list = list(clauses)
    with path.open("w", encoding="utf-8") as f:
        f.write(f"p cnf {nvars} {len(clause_list)}\n")
        for clause in clause_list:
            if clause:
                f.write(" ".join(clause) + " 0\n")
            else:
                f.write("0\n")


def _parse_dimacs_literal_cnf_text(text: str) -> tuple[int, List[List[int]]]:
    nvars = None
    declared_nclauses = None
    tokens: List[str] = []

    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith("c"):
            continue
        if line.startswith("p"):
            parts = line.split()
            if len(parts) < 4 or parts[1] != "cnf":
                raise ValueError(f"invalid DIMACS header: {line}")
            nvars = int(parts[2])
            declared_nclauses = int(parts[3])
            continue
        if line.startswith("x"):
            raise ValueError("expected plain CNF, but found XOR line")
        tokens.extend(line.split())

    if nvars is None or declared_nclauses is None:
        raise ValueError("DIMACS header not found")

    clauses: List[List[int]] = []
    current: List[int] = []
    for tok in tokens:
        if tok == "0":
            clauses.append(current)
            current = []
            continue
        lit = int(tok)
        if lit == 0 or abs(lit) > nvars:
            raise ValueError(f"literal out of range: {lit}")
        current.append(lit)
    if current:
        raise ValueError("last clause missing trailing 0")
    if len(clauses) != declared_nclauses:
        raise ValueError(f"header clause count mismatch: expected {declared_nclauses}, got {len(clauses)}")
    return nvars, clauses


def _lift_cnf_literals_to_xnf(nvars: int, clauses: Sequence[Sequence[int]], lift_k: int) -> tuple[int, List[List[str]]]:
    if lift_k < 1:
        raise ValueError("lift_k must be >= 1")

    def block(v: int) -> List[int]:
        start = (v - 1) * lift_k + 1
        return list(range(start, start + lift_k))

    lifted_clauses: List[List[str]] = []
    for clause in clauses:
        lifted_clause: List[str] = []
        for lit in clause:
            v = abs(lit)
            body = "+".join(str(x) for x in block(v))
            lifted_clause.append(body if lit > 0 else "-" + body)
        lifted_clauses.append(lifted_clause)
    return nvars * lift_k, lifted_clauses


def _write_dimacs_text(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8") as f:
        if text.endswith("\n"):
            f.write(text)
        else:
            f.write(text + "\n")


def generate_dense_kxnf(n: int, k: int, out: Path, *, seed: int | None = None) -> int:
    rng = random.Random(seed)
    m = _expected_kxnf_clause_count(n, k, fixed_equations=0)
    clauses: List[List[str]] = []
    for _ in range(m):
        tokens = []
        for _ in range(k):
            vars_ = _sample_dense_subset_nonempty(n, rng)
            rhs_zero = bool(rng.getrandbits(1))
            tokens.append(_format_equation(vars_, rhs_zero))
        clauses.append(tokens)
    _write_xnf(out, n, clauses)
    return m


def generate_restricted_kxnf(n: int, k: int, out: Path, *, seed: int | None = None) -> tuple[int, int]:
    rng = random.Random(seed)
    eq_count = n // 2
    m = _expected_kxnf_clause_count(n, k, fixed_equations=eq_count)
    clauses: List[List[str]] = []

    for _ in range(eq_count):
        vars_ = _sample_dense_subset_nonempty(n, rng)
        rhs_zero = bool(rng.getrandbits(1))
        clauses.append([_format_equation(vars_, rhs_zero)])

    for _ in range(m):
        tokens = []
        for _ in range(k):
            vars_ = _sample_dense_subset_nonempty(n, rng)
            rhs_zero = bool(rng.getrandbits(1))
            tokens.append(_format_equation(vars_, rhs_zero))
        clauses.append(tokens)

    _write_xnf(out, n, clauses)
    return eq_count, m


def generate_pebbling_lifted(height: int, lift_k: int, out: Path) -> tuple[int, int]:
    if PebblingFormula is None or dag_pyramid is None:
        raise RuntimeError("Missing dependencies: install cnfgen and networkx to generate pebbling families.")
    formula = PebblingFormula(dag_pyramid(height))
    nvars, clauses = _parse_dimacs_literal_cnf_text(formula.to_dimacs())
    lifted_nvars, lifted_clauses = _lift_cnf_literals_to_xnf(nvars, clauses, lift_k)
    _write_xnf(out, lifted_nvars, lifted_clauses)
    return nvars, len(clauses)


def generate_bphp(n: int, out: Path) -> tuple[int, int]:
    if PigeonholePrinciple is None:
        raise RuntimeError("Missing dependencies: install cnfgen and networkx to generate BPHP families.")
    if n < 1:
        raise ValueError("n must be >= 1")
    formula = PigeonholePrinciple(n + 1, n, functional=True, onto=True)
    text = formula.to_dimacs()
    _write_dimacs_text(out, text)
    nvars, clauses = _parse_dimacs_literal_cnf_text(text)
    return nvars, len(clauses)


def _random_odd_charges(n: int, rng: random.Random) -> List[bool]:
    charges = [bool(rng.getrandbits(1)) for _ in range(n)]
    if sum(1 for c in charges if c) % 2 == 0:
        idx = rng.randrange(n)
        charges[idx] = not charges[idx]
    return charges


def generate_tseitin(n: int, d: int, out: Path, *, seed: int | None = None) -> tuple[int, int]:
    if nx is None or Graph is None or TseitinFormula is None:
        raise RuntimeError("Missing dependencies: install cnfgen and networkx to generate Tseitin families.")
    if n < 1 or d < 1:
        raise ValueError("n and d must be >= 1")
    if d >= n:
        raise ValueError("require d < n for d-regular graph")
    if (n * d) % 2 != 0:
        raise ValueError("require n*d even for d-regular graph")

    rng = random.Random(seed)
    g_nx = nx.random_regular_graph(d, n, seed=rng.randrange(2**32))
    g_cnfgen = Graph.from_networkx(g_nx)
    charges = _random_odd_charges(n, rng)
    formula = TseitinFormula(g_cnfgen, charges=charges)
    text = formula.to_dimacs()
    _write_dimacs_text(out, text)
    nvars, clauses = _parse_dimacs_literal_cnf_text(text)
    return nvars, len(clauses)


def _seed_for(base_seed: int | None, *vals: int) -> int | None:
    if base_seed is None:
        return None
    h = base_seed
    for v in vals:
        h = ((h * 1_000_003) ^ v) & 0xFFFFFFFF
    return h


def _random_filename(family: str, x: int, attempt: int) -> str:
    if family.startswith("random_2xnf"):
        return f"random_2xnf_{x}_{attempt:02d}.cnf"
    if family.startswith("random_3xnf"):
        return f"random_3xnf_{x}_{attempt:02d}.cnf"
    if family.startswith("random_6xnf"):
        return f"random_6xnf_{x}_{attempt:02d}.cnf"
    if family == "random_kxnf_15":
        return f"random_kxnf_15_{x}_{attempt:02d}.cnf"
    if family.startswith("restricted_2xnf"):
        return f"restricted_2xnf_{x}_{attempt:02d}.cnf"
    if family.startswith("restricted_3xnf"):
        return f"restricted_3xnf_{x}_{attempt:02d}.cnf"
    if family.startswith("restricted_6xnf"):
        return f"restricted_6xnf_{x}_{attempt:02d}.cnf"
    if family == "restricted_kxnf_25":
        return f"restricted_kxnf_25_{x}_{attempt:02d}.cnf"
    if family == "tseitin_k_20":
        return f"tseitin_k_20_{x}_{attempt:02d}.cnf"
    if family == "tseitin_5_n":
        return f"tseitin_5_n_{x}_{attempt:02d}.cnf"
    raise ValueError(f"unknown random family: {family}")


def _deterministic_filename(family: str, x: int) -> str:
    if family == "pebbling_2lift_h":
        return f"pebbling_2lift_{x}.cnf"
    if family == "pebbling_3lift_h":
        return f"pebbling_3lift_{x}.cnf"
    if family == "pebbling_10lift_h":
        return f"pebbling_10lift_{x}.cnf"
    if family == "pebbling_klift_100":
        return f"pebbling_klift_100_{x}.cnf"
    if family == "bijphp_n_n+1":
        return f"bijphp_{x}.cnf"
    raise ValueError(f"unknown deterministic family: {family}")


def _generate_instance(family: str, x: int, out: Path, seed: int | None) -> None:
    if family == "random_2xnf_n":
        generate_dense_kxnf(x, 2, out, seed=seed)
    elif family == "random_3xnf_n":
        generate_dense_kxnf(x, 3, out, seed=seed)
    elif family == "random_6xnf_n":
        generate_dense_kxnf(x, 6, out, seed=seed)
    elif family == "random_kxnf_15":
        generate_dense_kxnf(15, x, out, seed=seed)
    elif family == "restricted_2xnf_n":
        generate_restricted_kxnf(x, 2, out, seed=seed)
    elif family == "restricted_3xnf_n":
        generate_restricted_kxnf(x, 3, out, seed=seed)
    elif family == "restricted_6xnf_n":
        generate_restricted_kxnf(x, 6, out, seed=seed)
    elif family == "restricted_kxnf_25":
        generate_restricted_kxnf(25, x, out, seed=seed)
    elif family == "pebbling_2lift_h":
        generate_pebbling_lifted(x, 2, out)
    elif family == "pebbling_3lift_h":
        generate_pebbling_lifted(x, 3, out)
    elif family == "pebbling_10lift_h":
        generate_pebbling_lifted(x, 10, out)
    elif family == "pebbling_klift_100":
        generate_pebbling_lifted(100, x, out)
    elif family == "bijphp_n_n+1":
        generate_bphp(x, out)
    elif family == "tseitin_k_20":
        generate_tseitin(20, x, out, seed=seed)
    elif family == "tseitin_5_n":
        generate_tseitin(x, 5, out, seed=seed)
    else:
        raise ValueError(f"unknown family: {family}")


def generate_family(out_root: Path, spec: FamilySpec, attempts: int, seed_base: int | None) -> None:
    family_dir = out_root / spec.name
    family_dir.mkdir(parents=True, exist_ok=True)
    for old in family_dir.glob("*.cnf"):
        old.unlink()

    for x in spec.x_values:
        if spec.kind == "random":
            for attempt in range(1, attempts + 1):
                out = family_dir / _random_filename(spec.name, x, attempt)
                seed = _seed_for(seed_base, len(spec.name), x, attempt)
                _generate_instance(spec.name, x, out, seed)
        else:
            out = family_dir / _deterministic_filename(spec.name, x)
            _generate_instance(spec.name, x, out, seed=None)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Generate benchmark families for type1/type2 benchmark scripts.")
    parser.add_argument("--out-dir", default="tests", help="Root output directory (default: tests)")
    parser.add_argument("--attempts", type=int, default=5, help="Instances per size for random families (default: 5)")
    parser.add_argument("--seed-base", type=int, default=1, help="Base seed for random generators (default: 1)")
    parser.add_argument(
        "--families",
        nargs="*",
        choices=sorted(FAMILIES.keys()),
        default=sorted(FAMILIES.keys()),
        help="Subset of families to generate",
    )
    args = parser.parse_args()
    if args.attempts < 1:
        parser.error("--attempts must be >= 1")
    return args


def main() -> int:
    args = parse_args()
    out_root = Path(args.out_dir)
    out_root.mkdir(parents=True, exist_ok=True)

    for family in args.families:
        spec = FAMILIES[family]
        generate_family(out_root, spec, attempts=args.attempts, seed_base=args.seed_base)
        print(f"[{family}] generated {len(spec.x_values)} sizes (kind={spec.kind}) -> {out_root / family}")

    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (RuntimeError, ValueError, FileNotFoundError) as exc:
        print(f"Error: {exc}")
        raise SystemExit(2)
