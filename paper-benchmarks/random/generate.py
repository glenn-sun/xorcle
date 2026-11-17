#!/usr/bin/env python3
"""Generate the random XNF instances of the scaling runs, each in every encoding a solver reads.

    generate.py [--lines L,...] [--seeds 101-110] [--jobs 4]
    generate.py --check DIR [--jobs 4]

A line of `lines.csv` is random k-XNF at one k (equations per clause): m clauses of k
equations, each equation with a uniformly random constant and, for a dense line (v = 0), a
uniformly random nonempty subset of the n variables, or for a sparse line exactly v distinct
variables. For each line, each n from `n_start` to `n_max` in steps of `n_step`, and each
seed, this writes `instances/scaling/<line>/<name>.xnf` at m*(n) = round(a n + b) clauses, a
and b from `../results/random/thresholds.csv` (`threshold.py fit`), then its `.xcnf` and `.cnf`:

- `.xcnf`, CNF-XOR, for CryptoMiniSat: Mate Soos's `xnf_to_xcnf.py` (msoos/xnf-tests), as he
  gives CryptoMiniSat native XNF families. An equation of one variable stays a literal; every
  other becomes a new variable tied to it by an `x` line, one per distinct equation.
- `.cnf`, for Kissat and CaDiCaL: `Xnf.asCnf(5)` of anf_to_2xnf, the encoding of Danner's
  distributed `.cnf` files. The CNF-XOR's equations are cut into pieces of at most 5 variables
  with new variables, each piece written as the clauses forbidding its wrong parity.

Both keep the XNF's variables 1..n and number their own after n. `../setup_solvers.sh
encoders` fetches the two upstream files into `encoders/`. `instances.csv` lists every
instance with the sha256 of its three files and the Python that wrote them: the draws follow
`random`, and the blasted encoding's piece order follows CPython's set order, so another
version may write other files. A rerun skips the files that exist.

`--check DIR` regenerates every row of `instances.csv` under DIR and compares the three sha256.
"""

from __future__ import annotations

import argparse
import csv
import importlib.util
import os
import platform
import random
import subprocess
import sys
import threading
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
from math import log, log1p
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
import measure

LINES_CSV = HERE / "lines.csv"
INSTANCES = HERE / "instances"
INSTANCES_CSV = HERE / "instances.csv"
ENCODERS = HERE / "encoders"
RESULTS = measure.RESULTS / "random"
THRESHOLDS_CSV = RESULTS / "thresholds.csv"
FIELDS = ("line", "n", "m", "seed", "dir", "name", "xnf_sha256", "xcnf_sha256", "cnf_sha256", "python")
CUTTING_LENGTH = 5


@dataclass(frozen=True)
class Line:
    name: str
    k: int
    v: int
    sweep_n: tuple[int, ...]
    n_start: int
    n_step: int
    n_max: int

    @property
    def c(self) -> float:
        """-ln(1 - 2^-k): the expected solution count falls by e^-c per clause."""
        return -log1p(-(2.0 ** -self.k))

    def formula_a_b(self) -> tuple[float, float]:
        """The first-moment threshold m = a n + b, at which 2^n (1 - 2^-k)^m = ln 2."""
        return log(2.0) / self.c, -log(log(2.0)) / self.c

    def formula_m(self, n: int) -> float:
        a, b = self.formula_a_b()
        return a * n + b

    def scaling_ns(self) -> list[int]:
        return list(range(self.n_start, self.n_max + 1, self.n_step))


def load_lines(names: str = "") -> dict[str, Line]:
    """The lines of `lines.csv`, or only the comma-separated `names`."""
    with LINES_CSV.open() as f:
        lines = {r["line"]: Line(r["line"], int(r["k"]), int(r["v"]), tuple(int(x) for x in r["sweep_n"].split(";")),
                                 int(r["n_start"]), int(r["n_step"]), int(r["n_max"])) for r in csv.DictReader(f)}
    if not names:
        return lines
    unknown = [w for w in names.split(",") if w not in lines]
    if unknown:
        raise SystemExit(f"unknown lines: {', '.join(unknown)}; lines.csv has {', '.join(lines)}")
    return {w: lines[w] for w in names.split(",")}


def load_thresholds() -> dict[str, tuple[float, float]]:
    """Each line's fitted (a, b)."""
    if not THRESHOLDS_CSV.exists():
        raise SystemExit(f"{THRESHOLDS_CSV} is missing: run threshold.py fit first")
    with THRESHOLDS_CSV.open() as f:
        return {r["line"]: (float(r["a"]), float(r["b"])) for r in csv.DictReader(f)}


def parse_seeds(spec: str) -> list[int]:
    """`101-110,121` as [101, ..., 110, 121]."""
    seeds: list[int] = []
    for part in spec.split(","):
        lo, _, hi = part.partition("-")
        seeds.extend(range(int(lo), int(hi or lo) + 1))
    return seeds


# ---------------------------------------------------------------------------
# The XNF
# ---------------------------------------------------------------------------


def stem(line: Line, n: int, m: int, seed: int) -> str:
    return f"random_kxnf_n{n}_k{line.k}_m{m}" + (f"_v{line.v}" if line.v else "") + f"_s{seed}"


def xnf_text(line: Line, n: int, m: int, seed: int) -> str:
    """The instance's XNF: an equation is its variables joined by `+`, prefixed `-` when its constant is 0."""
    rng = random.Random(seed)
    out = [f"p cnf {n} {m}\n"]
    for _ in range(m):
        tokens = []
        for _ in range(line.k):
            if line.v:
                vars_ = sorted(rng.sample(range(1, n + 1), line.v))
            else:
                while not (vars_ := [i + 1 for i in range(n) if rng.getrandbits(1)]):
                    pass
            tokens.append(("-" if rng.getrandbits(1) else "") + "+".join(map(str, vars_)))
        out.append(" ".join(tokens) + " 0\n")
    return "".join(out)


def write_xnf(line: Line, n: int, m: int, seed: int, out_dir: Path) -> Path:
    """The XNF of (line, n, m, seed) under `out_dir`, written unless already there."""
    out = out_dir / f"{stem(line, n, m, seed)}.xnf"
    if not out.exists():
        out_dir.mkdir(parents=True, exist_ok=True)
        tmp = out.with_suffix(".xnf.part")
        tmp.write_text(xnf_text(line, n, m, seed))
        tmp.replace(out)
    return out


# ---------------------------------------------------------------------------
# The encodings
# ---------------------------------------------------------------------------


def _encoder(path: Path) -> Path:
    if not path.exists():
        raise SystemExit(f"{path} is missing: run ../setup_solvers.sh encoders first")
    return path


def write_xcnf(xnf: Path, out: Path) -> None:
    spec = importlib.util.spec_from_file_location("xnf_to_xcnf", _encoder(ENCODERS / "xnf_to_xcnf.py"))
    soos = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(soos)
    nvars, clauses = soos.parse_xnf(str(xnf))
    total_vars, lines = soos.convert(nvars, clauses, reuse=True)
    tmp = out.with_suffix(out.suffix + ".part")
    tmp.write_text(f"p cnf {total_vars} {len(lines)}\n" + "\n".join(lines) + "\n")
    tmp.replace(out)


def write_cnf(xnf: Path, out: Path) -> None:
    """Blast in a fresh interpreter: anf_to_2xnf keeps module-level state between formulas."""
    _encoder(ENCODERS / "anf_to_2xnf" / "xnf.py")
    env = dict(os.environ, OPENBLAS_NUM_THREADS="1", OMP_NUM_THREADS="1", MKL_NUM_THREADS="1")
    subprocess.run([sys.executable, "-I", str(Path(__file__).resolve()), "--blast", str(xnf), str(out)],
                   check=True, env=env)


def _blast(xnf_path: Path, out: Path) -> None:
    sys.path.insert(0, str(ENCODERS / "anf_to_2xnf"))
    sys.setrecursionlimit(100000)
    import xnf

    formula = xnf.Xnf(xnf_path.read_text())
    # anf_to_2xnf numbers its new variables after the largest one in a clause, not after the
    # p line's n; the two differ only when a variable occurs in no clause.
    with xnf_path.open() as f:
        n = next(int(s.split()[2]) for s in f if s.startswith("p "))
    formula.numVars = max(formula.numVars, n)
    tmp = out.with_suffix(out.suffix + ".part")
    tmp.write_text(formula.asCnf(CUTTING_LENGTH) + "\n")
    tmp.replace(out)


def write_all(line: Line, n: int, m: int, seed: int, out_dir: Path) -> dict:
    """The instance in all three encodings under `out_dir`, and its `instances.csv` row."""
    xnf = write_xnf(line, n, m, seed, out_dir)
    xcnf, cnf = xnf.with_suffix(".xcnf"), xnf.with_suffix(".cnf")
    if not xcnf.exists():
        write_xcnf(xnf, xcnf)
    if not cnf.exists():
        write_cnf(xnf, cnf)
    return {"line": line.name, "n": n, "m": m, "seed": seed, "dir": os.path.relpath(out_dir, HERE),
            "name": xnf.stem, "xnf_sha256": measure.sha256(xnf), "xcnf_sha256": measure.sha256(xcnf),
            "cnf_sha256": measure.sha256(cnf), "python": platform.python_version()}


def read_rows() -> dict[tuple[str, int, int], dict]:
    if not INSTANCES_CSV.exists():
        return {}
    with INSTANCES_CSV.open() as f:
        return {(r["line"], int(r["n"]), int(r["seed"])): r for r in csv.DictReader(f)}


def pool(todo: list, one, jobs: int) -> None:
    lock = threading.Lock()
    done = [0]

    def wrapped(item) -> None:
        msg = one(item)
        with lock:
            done[0] += 1
            if done[0] % 50 == 0 or done[0] == len(todo) or msg:
                print(f"[{done[0]}/{len(todo)}] {msg or ''}", flush=True)

    with ThreadPoolExecutor(jobs) as ex:
        list(ex.map(wrapped, todo))


def check(out_root: Path, jobs: int) -> int:
    lines = load_lines()
    rows = list(read_rows().values())
    bad = []

    def one(r: dict) -> str:
        got = write_all(lines[r["line"]], int(r["n"]), int(r["m"]), int(r["seed"]), out_root / r["line"])
        diff = [f for f in ("name", "xnf_sha256", "xcnf_sha256", "cnf_sha256") if got[f] != r[f]]
        if diff:
            bad.append((r["name"], diff))
            return f"MISMATCH {r['name']}: {', '.join(diff)}"
        return ""

    pool(rows, one, jobs)
    print(f"{len(rows)} rows regenerated, {len(rows) - len(bad)} identical in all three files, {len(bad)} differ")
    return 1 if bad else 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--lines", default="", help="comma-separated lines of lines.csv; default all")
    ap.add_argument("--seeds", default="101-110")
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--check", type=Path, metavar="DIR", help="regenerate every row of instances.csv under DIR and compare")
    ap.add_argument("--blast", nargs=2, type=Path, help=argparse.SUPPRESS)
    args = ap.parse_args()
    if args.blast:
        _blast(*args.blast)
        return 0
    if args.check:
        return check(args.check, args.jobs)

    lines = load_lines(args.lines)
    thresholds = load_thresholds()
    rows = read_rows()
    others = {r["python"] for r in rows.values()} - {platform.python_version()}
    if others:
        print(f"warning: instances.csv was written by Python {', '.join(sorted(others))}, this is "
              f"{platform.python_version()}; every file written is checked against its listed sha256",
              file=sys.stderr, flush=True)
    todo = []
    for name, line in lines.items():
        a, b = thresholds[name]
        for n in line.scaling_ns():
            m = round(a * n + b)
            for seed in parse_seeds(args.seeds):
                old = rows.get((name, n, seed))
                if old and int(old["m"]) != m:
                    raise SystemExit(f"{name} n={n} seed={seed}: instances.csv has m={old['m']}, the fit gives {m}")
                todo.append((line, n, m, seed))
    lock = threading.Lock()

    def one(item) -> str:
        line, n, m, seed = item
        row = write_all(line, n, m, seed, INSTANCES / "scaling" / line.name)
        with lock:
            old = rows.get((row["line"], row["n"], row["seed"]))
            if old:
                for ext in ("xnf", "xcnf", "cnf"):
                    if old[f"{ext}_sha256"] != row[f"{ext}_sha256"]:
                        raise SystemExit(f"{row['dir']}/{row['name']}.{ext} does not match its sha256 in "
                                         f"instances.csv (listed as written by Python {old['python']})")
            else:
                rows[(row["line"], row["n"], row["seed"])] = row
        return ""

    pool(todo, one, args.jobs)
    order = list(load_lines())
    with INSTANCES_CSV.open("w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=FIELDS)
        w.writeheader()
        w.writerows(sorted(rows.values(), key=lambda r: (order.index(r["line"]), int(r["n"]), int(r["seed"]))))
    print(f"{len(todo)} instances ready; {len(rows)} rows in {INSTANCES_CSV}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
