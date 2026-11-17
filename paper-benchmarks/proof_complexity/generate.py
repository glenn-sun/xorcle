#!/usr/bin/env python3
"""Generate the proof-complexity instances: Tseitin, perfect matching and bijective pigeonhole formulas.

    generate.py LINE N           one point, written and listed in instances.csv if not already
    generate.py --check [--jobs 4]

Each formula is cnfgen's CNF, with its clause lines then shuffled:

- `tseitin` (n, k): the Tseitin formula on a random k-regular graph on n vertices, with
  random charges of odd total, so unsatisfiable.
- `perfect_matching` (n, k): the perfect matching principle on a random k-regular graph on n
  vertices; n is odd, so unsatisfiable.
- `bijective_pigeonhole` (n, k): the perfect matching principle on the complete bipartite
  graph of n + k pigeons and n holes.

The random graphs and charges are drawn from `random.Random(1)`: first the seed of networkx's
`random_regular_graph`, then the charges. bijective_pigeonhole draws nothing and is listed
with seed 0. cnfgen's file is written to `instances/generator/<family>/`, and its clause
lines, shuffled by `random.Random(1).shuffle` with the `c` and `p` lines kept on top, to
`instances/shuffle-1/<family>/`, which the solvers read. networkx's random graphs may differ
in another version, so only cnfgen 0.9.5 and networkx 3.6.1 are accepted.

`instances.csv` lists every point the runs used, with both files' sha256, the shuffled file's
variable and clause counts, and the versions that wrote it. `--check` regenerates every row
and compares.
"""

from __future__ import annotations

import argparse
import csv
import importlib.metadata
import os
import platform
import random
import sys
import threading
from collections.abc import Iterator
from concurrent.futures import ProcessPoolExecutor
from dataclasses import dataclass
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
import measure

LINES_CSV = HERE / "lines.csv"
INSTANCES = HERE / "instances"
INSTANCES_CSV = HERE / "instances.csv"
VERSIONS = {"cnfgen": "0.9.5", "networkx": "3.6.1"}
GRAPH_SEED = 1
SHUFFLE_SEED = 1
FIELDS = ("family", "k", "n", "seed", "shuffle_seed", "dir", "name", "generated_sha256", "cnf_sha256", "vars",
          "clauses", "python", "cnfgen", "networkx")
ROUND = (10, 15, 20, 30, 50, 70)


@dataclass(frozen=True)
class Line:
    """A family at one k, whose n run from `n_start` through `series` up to `n_max`."""

    name: str
    family: str
    k: int
    series: str
    n_start: int
    n_max: int

    def ns(self) -> Iterator[int]:
        """The n of the line: the round numbers 10, 15, 20, 30, 50, 70 times a power of ten (`round`), the same
        plus one where even (`round_odd`), or every integer (`consecutive`), leaving out the n the family cannot take."""
        if self.series == "consecutive":
            values: Iterator[int] = iter(range(self.n_start, self.n_max + 1))
        else:
            odd = self.series == "round_odd"
            values = (c * 10**j | odd for j in range(len(str(self.n_max))) for c in ROUND)
        for n in sorted(set(values)):
            if self.n_start <= n <= self.n_max and self.takes(n):
                yield n

    def takes(self, n: int) -> bool:
        if self.family == "bijective_pigeonhole":
            return n >= 1
        return self.k < n and n * self.k % 2 == 0

    @property
    def seed(self) -> int:
        return 0 if self.family == "bijective_pigeonhole" else GRAPH_SEED

    def stem(self, n: int) -> str:
        return f"{self.family}_n{n}_k{self.k}" + (f"_s{self.seed}" if self.seed else "")


def load_lines() -> dict[str, Line]:
    with LINES_CSV.open() as f:
        return {r["line"]: Line(r["line"], r["family"], int(r["k"]), r["series"], int(r["n_start"]), int(r["n_max"]))
                for r in csv.DictReader(f)}


def versions() -> dict[str, str]:
    try:
        got = {name: importlib.metadata.version(name) for name in VERSIONS}
    except importlib.metadata.PackageNotFoundError as exc:
        raise SystemExit(f"{exc.name} is missing: pip install -r ../requirements.txt") from None
    if got != VERSIONS:
        raise SystemExit(f"cnfgen and networkx are {got}, the instances need {VERSIONS}")
    return {"python": platform.python_version(), **got}


def cnfgen_text(line: Line, n: int) -> str:
    import networkx as nx
    from cnfgen import PerfectMatchingPrinciple, TseitinFormula
    from cnfgen.graphs import Graph

    if line.family == "bijective_pigeonhole":
        formula = PerfectMatchingPrinciple(Graph.from_networkx(nx.complete_bipartite_graph(n + line.k, n)))
    else:
        rng = random.Random(line.seed)
        graph = Graph.from_networkx(nx.random_regular_graph(line.k, n, seed=rng.randrange(2**32)))
        if line.family == "tseitin":
            charges = [bool(rng.getrandbits(1)) for _ in range(n)]
            if sum(charges) % 2 == 0:
                i = rng.randrange(n)
                charges[i] = not charges[i]
            formula = TseitinFormula(graph, charges=charges)
        elif line.family == "perfect_matching":
            formula = PerfectMatchingPrinciple(graph)
        else:
            raise ValueError(f"unknown family {line.family}")
    text = formula.to_dimacs()
    return text if text.endswith("\n") else text + "\n"


def shuffled(text: str, seed: int) -> str:
    head, body = [], []
    for line in text.splitlines(keepends=True):
        if line.strip():
            (head if line[0] in "cp" else body).append(line)
    random.Random(seed).shuffle(body)
    return "".join(head + body)


def write(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(".part")
    tmp.write_text(text)
    tmp.replace(path)


def make(line: Line, n: int, root: Path = INSTANCES) -> dict:
    """Write the point's two files under `root` and return its row."""
    text = cnfgen_text(line, n)
    name = line.stem(n)
    generated = root / "generator" / line.family / f"{name}.cnf"
    out = root / f"shuffle-{SHUFFLE_SEED}" / line.family / f"{name}.cnf"
    write(generated, text)
    body = shuffled(text, SHUFFLE_SEED)
    write(out, body)
    p = next(s for s in body.splitlines() if s.startswith("p ")).split()
    return {"family": line.family, "k": line.k, "n": n, "seed": line.seed, "shuffle_seed": SHUFFLE_SEED,
            "dir": os.path.relpath(out.parent, HERE), "name": name, "generated_sha256": measure.sha256(generated),
            "cnf_sha256": measure.sha256(out), "vars": int(p[2]), "clauses": int(p[3]), **versions()}


def read_rows() -> dict[tuple[str, int, int], dict]:
    if not INSTANCES_CSV.exists():
        return {}
    with INSTANCES_CSV.open() as f:
        return {(r["family"], int(r["k"]), int(r["n"])): r for r in csv.DictReader(f)}


_lock = threading.Lock()
_point_locks: dict[tuple, threading.Lock] = {}


def point(line: Line, n: int) -> dict:
    """The instances.csv row of (line, n), its files written first if missing; a new row is appended."""
    key = (line.family, line.k, n)
    with _lock:
        lock = _point_locks.setdefault(key, threading.Lock())
    with lock:
        row = read_rows().get(key)
        if row and (HERE / row["dir"] / f"{row['name']}.cnf").exists():
            return row
        new = make(line, n)
        if row:
            if any(str(new[f]) != row[f] for f in ("generated_sha256", "cnf_sha256")):
                raise SystemExit(f"{line.stem(n)} was regenerated with another sha256 than instances.csv lists")
            return row
        with _lock:
            fresh = not INSTANCES_CSV.exists()
            with INSTANCES_CSV.open("a", newline="") as f:
                w = csv.DictWriter(f, fieldnames=FIELDS)
                if fresh:
                    w.writeheader()
                w.writerow(new)
        return {k: str(v) for k, v in new.items()}


def check(jobs: int) -> int:
    lines = {(line.family, line.k): line for line in load_lines().values()}
    rows = list(read_rows().values())
    bad = []
    with ProcessPoolExecutor(jobs) as ex:
        made = ex.map(make, [lines[(r["family"], int(r["k"]))] for r in rows], [int(r["n"]) for r in rows])
        for r, got in zip(rows, made):
            diff = [f for f in ("name", "generated_sha256", "cnf_sha256", "vars", "clauses") if str(got[f]) != r[f]]
            if diff:
                bad.append(r["name"])
                print(f"MISMATCH {r['name']}: {', '.join(diff)}", flush=True)
    print(f"{len(rows)} rows regenerated, {len(rows) - len(bad)} identical, {len(bad)} differ")
    return 1 if bad else 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("line", nargs="?")
    ap.add_argument("n", nargs="?", type=int)
    ap.add_argument("--check", action="store_true", help="regenerate every row of instances.csv and compare")
    ap.add_argument("--jobs", type=int, default=4)
    args = ap.parse_args()
    versions()
    if args.check:
        return check(args.jobs)
    if args.line is None or args.n is None:
        ap.error("give LINE and N, or --check")
    line = load_lines()[args.line]
    if not line.takes(args.n):
        raise SystemExit(f"{args.line} cannot take n={args.n}")
    print(point(line, args.n))
    return 0


if __name__ == "__main__":
    sys.exit(main())
