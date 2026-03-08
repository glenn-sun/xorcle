#!/usr/bin/env python3
"""Generate benchmark families for parity-sat-cpp."""

from __future__ import annotations

import argparse
import csv
import random
from dataclasses import dataclass
from math import log, log1p
from pathlib import Path
from typing import Iterable, List, Optional, Sequence

try:
    import networkx as nx
    from cnfgen import PebblingFormula, TseitinFormula
    from cnfgen.graphs import Graph, dag_pyramid
except ModuleNotFoundError:
    nx = None  # type: ignore[assignment]
    PebblingFormula = None  # type: ignore[assignment]
    TseitinFormula = None  # type: ignore[assignment]
    Graph = None  # type: ignore[assignment]
    dag_pyramid = None  # type: ignore[assignment]

from benchmark_orchestration import GENERATED_FAMILIES, MANIFEST_FIELDS


@dataclass(frozen=True)
class GeneratedFile:
    family: str
    file: Path
    input_format: str
    attempt: int
    n: Optional[int] = None
    k: Optional[int] = None
    h: Optional[int] = None
    lift_k: Optional[int] = None


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
    m = ((n - fixed_equations) * log(2.0) - log(log(2.0))) / denom
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


def _seed_for(base_seed: int, *vals: int) -> int:
    h = base_seed
    for v in vals:
        h = ((h * 1_000_003) ^ v) & 0xFFFFFFFF
    return h


def _write_manifest(family_dir: Path, tests_root: Path, rows: Sequence[GeneratedFile]) -> None:
    manifest_path = family_dir / "manifest.csv"
    with manifest_path.open("w", encoding="utf-8", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=list(MANIFEST_FIELDS))
        writer.writeheader()
        for row in rows:
            writer.writerow(
                {
                    "file": str(row.file.relative_to(tests_root)),
                    "family": row.family,
                    "input_format": row.input_format,
                    "attempt": str(row.attempt),
                    "n": "" if row.n is None else str(row.n),
                    "k": "" if row.k is None else str(row.k),
                    "h": "" if row.h is None else str(row.h),
                    "lift_k": "" if row.lift_k is None else str(row.lift_k),
                }
            )


def _clear_family_dir(path: Path) -> None:
    path.mkdir(parents=True, exist_ok=True)
    for old in path.glob("*"):
        if old.is_file():
            old.unlink()


def _make_out_path(out_root: Path, family: str, stem: str) -> Path:
    return out_root / family / f"{stem}.cnf"


def generate_random_kxnf_family(out_root: Path, tests_root: Path, seed_base: int) -> int:
    family = "random_kxnf_n"
    family_dir = out_root / family
    _clear_family_dir(family_dir)
    manifest_rows: List[GeneratedFile] = []
    for k in range(2, 7):
        for n in range(11, 31):
            file_path = _make_out_path(out_root, family, f"{family}_k{k}_n{n}")
            seed = _seed_for(seed_base, len(family), k, n, 1)
            generate_dense_kxnf(n, k, file_path, seed=seed)
            manifest_rows.append(
                GeneratedFile(
                    family=family,
                    file=file_path,
                    input_format="xnf",
                    attempt=1,
                    n=n,
                    k=k,
                )
            )
    _write_manifest(family_dir, tests_root, manifest_rows)
    return len(manifest_rows)


def generate_restricted_kxnf_family(out_root: Path, tests_root: Path, seed_base: int) -> int:
    family = "restricted_kxnf_n"
    family_dir = out_root / family
    _clear_family_dir(family_dir)
    manifest_rows: List[GeneratedFile] = []
    for k in range(2, 7):
        for n in range(22, 61, 2):
            file_path = _make_out_path(out_root, family, f"{family}_k{k}_n{n}")
            seed = _seed_for(seed_base, len(family), k, n, 1)
            generate_restricted_kxnf(n, k, file_path, seed=seed)
            manifest_rows.append(
                GeneratedFile(
                    family=family,
                    file=file_path,
                    input_format="xnf",
                    attempt=1,
                    n=n,
                    k=k,
                )
            )
    _write_manifest(family_dir, tests_root, manifest_rows)
    return len(manifest_rows)


def generate_pebbling_family(out_root: Path, tests_root: Path) -> int:
    family = "pebbling_lifted_h_k"
    family_dir = out_root / family
    _clear_family_dir(family_dir)
    manifest_rows: List[GeneratedFile] = []
    for lift_k in (2, 4, 6, 8, 10):
        for h in range(60, 251, 10):
            file_path = _make_out_path(out_root, family, f"{family}_h{h}_k{lift_k}")
            generate_pebbling_lifted(h, lift_k, file_path)
            manifest_rows.append(
                GeneratedFile(
                    family=family,
                    file=file_path,
                    input_format="xnf",
                    attempt=1,
                    h=h,
                    k=lift_k,
                    lift_k=lift_k,
                )
            )
    _write_manifest(family_dir, tests_root, manifest_rows)
    return len(manifest_rows)


def generate_tseitin_family(out_root: Path, tests_root: Path, seed_base: int) -> int:
    family = "tseitin_n_k"
    family_dir = out_root / family
    _clear_family_dir(family_dir)
    manifest_rows: List[GeneratedFile] = []

    param_pairs = {(20, k) for k in range(3, 11)}
    param_pairs.update((n, 4) for n in (10, 20, 40, 80, 160, 320, 640, 1280))

    for n, k in sorted(param_pairs):
        for attempt in range(1, 6):
            file_path = _make_out_path(out_root, family, f"{family}_n{n}_k{k}_{attempt:02d}")
            seed = _seed_for(seed_base, len(family), n, k, attempt)
            generate_tseitin(n, k, file_path, seed=seed)
            manifest_rows.append(
                GeneratedFile(
                    family=family,
                    file=file_path,
                    input_format="cnf",
                    attempt=attempt,
                    n=n,
                    k=k,
                )
            )
    _write_manifest(family_dir, tests_root, manifest_rows)
    return len(manifest_rows)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Generate benchmark families for the benchmark scripts.")
    parser.add_argument("--out-dir", default="tests/generated", help="Root output directory (default: tests/generated)")
    parser.add_argument("--seed-base", type=int, default=1, help="Base seed for random generators (default: 1)")
    parser.add_argument(
        "--families",
        nargs="*",
        choices=list(GENERATED_FAMILIES),
        default=list(GENERATED_FAMILIES),
        help="Subset of families to generate",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    out_root = Path(args.out_dir).resolve()
    out_root.mkdir(parents=True, exist_ok=True)
    tests_root = out_root.parent.resolve()

    counts = {
        "random_kxnf_n": lambda: generate_random_kxnf_family(out_root, tests_root, args.seed_base),
        "restricted_kxnf_n": lambda: generate_restricted_kxnf_family(out_root, tests_root, args.seed_base),
        "pebbling_lifted_h_k": lambda: generate_pebbling_family(out_root, tests_root),
        "tseitin_n_k": lambda: generate_tseitin_family(out_root, tests_root, args.seed_base),
    }

    for family in args.families:
        count = counts[family]()
        print(f"[{family}] generated {count} files -> {out_root / family}")

    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (RuntimeError, ValueError, FileNotFoundError) as exc:
        print(f"Error: {exc}")
        raise SystemExit(2)
