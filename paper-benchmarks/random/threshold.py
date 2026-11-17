#!/usr/bin/env python3
"""Find each line's satisfiability threshold m*(n), the clause count where P(SAT) = 1/2.

    threshold.py probe [--lines L,...] [--n-from 10] [--per-n 5] [--target-s 1] [--timeout 60]
    threshold.py run   [--lines L,...] [--per-m 30] [--timeout 600]
    threshold.py fit   [--lines L,...]
    threshold.py certify [--jobs 8] [--proof-dir DIR] [--checker PATH] [--proof-timeout 600] [--check-timeout 1200]
    common:            [--jobs 8] [--sparse-ratio 0.95] [--scratch DIR] [--perf PATH]

Every instance is solved by Xorcle v0.2 alone, through `../measure.py`, and the verdict is
all that is read. Results go to `../results/random/threshold/` and, from `fit`,
`../results/random/thresholds.csv`.

`probe` finds the n at which a line's instances near the threshold take Xorcle about a
second (by default for the sparse lines): from `--n-from` it grows n by a quarter at a time,
solving `--per-n` instances at `--sparse-ratio` times the first-moment formula's clause count,
until their median time reaches `--target-s`. It writes `probe.csv`, from which the sparse
lines' `sweep_n` in `lines.csv` were chosen.

`run` solves, at each n of the line's `sweep_n`, `--per-m` fresh instances at every m within
2 / c of `ratio x` the formula's m (ratio 1 for dense, `--sparse-ratio` for sparse), in
steps of max(1, round(w / 12)) where w = ln 22 / c is the first-moment estimate of the
90%-to-10% window, about 13 values across it. Each end of the range widens by 1 / c, in the
same steps, until P(SAT) is at least 0.9 at the low end and at most 0.1 at the high end. The i-th instance of (n, m) has seed 1001 + i + 1000 m.

`fit` fits a logistic curve of P(SAT) against m at each n by maximum likelihood, whose
midpoint is m50(n), then a line m50(n) = a n + b by least squares across n, and writes
`fits.csv` (per n) and `thresholds.csv` (per line, beside the formula's a and b), which
`generate.py` reads.

`certify` follows every UNSAT answer of `probe` and `run` with a proof run (`../measure.py`):
the same run with `--proof`, its proof checked by `check_lrup_parity` against the `.xnf` and
deleted, recorded in `threshold/proofs/xorcle-0.2.jsonl`; it exits 1 if a proof is refused.

All four resume: `probe.jsonl` and `runs.jsonl` hold one JSON line per run, and a run
already there is not repeated. A SAT answer's model goes to `threshold/models/`.
"""

from __future__ import annotations

import argparse
import csv
import json
import platform
import statistics
import sys
from math import ceil, exp, log, log1p, sqrt
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
import measure

import generate as G

SWEEP = G.RESULTS / "threshold"
PROBE_JSONL = SWEEP / "probe.jsonl"
PROBE_CSV = SWEEP / "probe.csv"
RUNS_JSONL = SWEEP / "runs.jsonl"
FITS_CSV = SWEEP / "fits.csv"
ARM = "xorcle-0.2"
#: P(SAT) the low and the high end of a range must reach.
P_LOW_END, P_HIGH_END = 0.9, 0.1
MAX_ROUNDS = 50
#: The i-th instance at (n, m) has seed 1001 + i + 1000 m, apart from the scaling runs' seeds 101-110.
SEED_BASE, SEEDS_PER_M = 1001, 1000


def sweep_seed(m: int, i: int) -> int:
    if not 0 <= i < SEEDS_PER_M:
        raise ValueError(f"at most {SEEDS_PER_M} instances per (n, m)")
    return SEED_BASE + i + SEEDS_PER_M * m


def read_records(path: Path) -> dict[tuple, dict]:
    if not path.exists():
        return {}
    recs = (json.loads(s) for s in path.read_text().splitlines() if s)
    return {(r["line"], r["n"], r["m"], r["seed"]): r for r in recs}


def solve_all(todo: list[tuple[G.Line, int, int, int]], path: Path, args) -> None:
    """Solve every (line, n, m, seed) of `todo` in a pool of `--jobs`, appending to `path`."""
    if not todo:
        return
    arm = measure.load_arms()[ARM]
    limits = measure.Limits(timeout=args.timeout, mem_mb=args.mem_mb, jobs=args.jobs, order_seed=args.order_seed,
                            perf=args.perf, scratch=Path(args.scratch))

    def execute(item: tuple[G.Line, int, int, int]) -> dict:
        line, n, m, seed = item
        xnf = G.write_xnf(line, n, m, seed, G.INSTANCES / "threshold" / line.name)
        model = SWEEP / "models" / ARM / f"{line.name}__{xnf.stem}.v.gz"
        rec = measure.run_one(ARM, arm, xnf.relative_to(measure.HERE), limits, model=model)
        if rec["verdict"] != "UNKNOWN":
            del rec["stdout_tail"], rec["stderr_tail"]
        return {"line": line.name, "n": n, "m": m, "seed": seed, "python": platform.python_version(), "arm": ARM,
                "family": line.name, "name": xnf.stem, "xnf_sha256": measure.sha256(xnf), **rec}

    def record(item: tuple, rec: dict) -> str:
        measure.append_jsonl(path, rec)
        return f"{rec['line']} n={rec['n']} m={rec['m']} seed={rec['seed']} {rec['verdict']} {rec['wall_s']} s"

    measure.run_flat(todo, execute, record, jobs=args.jobs, order_seed=args.order_seed)


def m_step(line: G.Line) -> int:
    """The step between swept clause counts: about 13 of them across the 90%-to-10% window, ln 22 / c."""
    return max(1, round(log(22) / line.c / 12))


def center_m(line: G.Line, n: int, args) -> int:
    return max(1, round((args.sparse_ratio if line.v else 1.0) * line.formula_m(n)))


# ---------------------------------------------------------------------------
# probe
# ---------------------------------------------------------------------------


def median_wall(recs: list[dict]) -> float:
    """Median wall time, a run without a verdict counting as longer than every other."""
    return statistics.median(r["wall_s"] if r["verdict"] != "UNKNOWN" else float("inf") for r in recs)


def probe(lines: dict[str, G.Line], args) -> None:
    active = {name: args.n_from for name in lines}
    finished: dict[str, str] = {}
    while active:
        batch = {name: [(n, center_m(lines[name], n, args), sweep_seed(center_m(lines[name], n, args), i))
                        for i in range(args.per_n)]
                 for name, n in active.items()}
        done = read_records(PROBE_JSONL)
        solve_all([(lines[name], n, m, s) for name, runs in batch.items() for n, m, s in runs
                   if (name, n, m, s) not in done], PROBE_JSONL, args)
        done = read_records(PROBE_JSONL)
        for name, runs in batch.items():
            n = active[name]
            t = median_wall([done[(name, n, m, s)] for n, m, s in runs])
            if t >= args.target_s:
                finished[name] = f"median {t:.2f} s at n = {n}"
                del active[name]
            elif n >= args.n_limit:
                finished[name] = f"median {t:.2f} s at n = {n}, the limit"
                del active[name]
            else:
                active[name] = max(n + 1, round(n * 1.25))
    write_probe_csv()
    for name, why in finished.items():
        print(f"{name}: {why}")
    print(f"wrote {PROBE_CSV}")


def write_probe_csv() -> None:
    groups: dict[tuple, list[dict]] = {}
    for r in read_records(PROBE_JSONL).values():
        groups.setdefault((r["line"], r["n"], r["m"]), []).append(r)
    with PROBE_CSV.open("w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["line", "n", "m", "runs", "sat", "unsat", "unknown", "median_wall_s"])
        for (line, n, m), recs in sorted(groups.items()):
            verdicts = [r["verdict"] for r in recs]
            w.writerow([line, n, m, len(recs), verdicts.count("SAT"), verdicts.count("UNSAT"),
                        verdicts.count("UNKNOWN"), round(median_wall(recs), 3)])


# ---------------------------------------------------------------------------
# run
# ---------------------------------------------------------------------------


def p_sat(recs: list[dict]) -> float | None:
    """The share of SAT among the runs with a verdict, or None without one."""
    verdicts = [r["verdict"] for r in recs if r["verdict"] != "UNKNOWN"]
    return verdicts.count("SAT") / len(verdicts) if verdicts else None


def run(lines: dict[str, G.Line], args) -> None:
    ms: dict[tuple[str, int], set[int]] = {}
    for name, line in lines.items():
        step = m_step(line)
        half = step * ceil(2 / line.c / step)
        for n in line.sweep_n:
            c = center_m(line, n, args)
            ms[(name, n)] = {m for m in range(c - half, c + half + 1, step) if m >= 1}
    for _ in range(MAX_ROUNDS):
        done = read_records(RUNS_JSONL)
        for (name, n, m, _seed) in done:
            if (name, n) in ms:
                ms[(name, n)].add(m)
        todo = [(lines[name], n, m, sweep_seed(m, i)) for (name, n), mset in ms.items() for m in sorted(mset)
                for i in range(args.per_m) if (name, n, m, sweep_seed(m, i)) not in done]
        print(f"round: {len(todo)} runs over {len(ms)} (line, n)", flush=True)
        solve_all(todo, RUNS_JSONL, args)
        done = read_records(RUNS_JSONL)
        widened = False
        for (name, n), mset in ms.items():
            step = m_step(lines[name])
            half = step * ceil(1 / lines[name].c / step)
            by_m = {m: [done[(name, n, m, sweep_seed(m, i))] for i in range(args.per_m)] for m in mset}
            lo, hi = min(mset), max(mset)
            p_lo, p_hi = p_sat(by_m[lo]), p_sat(by_m[hi])
            if p_lo is not None and p_lo < P_LOW_END and lo > 1:
                mset.update(m for m in range(lo - half, lo, step) if m >= 1)
                widened = True
            if p_hi is not None and p_hi > P_HIGH_END:
                mset.update(range(hi + step, hi + half + 1, step))
                widened = True
        if not widened:
            print(f"every range reaches P(SAT) >= {P_LOW_END} and <= {P_HIGH_END}; run threshold.py fit")
            return
    raise SystemExit(f"ranges still widening after {MAX_ROUNDS} rounds")


# ---------------------------------------------------------------------------
# certify
# ---------------------------------------------------------------------------


def certify(args) -> int:
    lines = G.load_lines()
    proofs = SWEEP / "proofs" / f"{ARM}.jsonl"
    done = set(read_records(proofs))
    unsat = {key: r for path in (PROBE_JSONL, RUNS_JSONL) for key, r in read_records(path).items()
             if r["verdict"] == "UNSAT"}
    todo = [r for key, r in unsat.items() if key not in done]
    if todo and not Path(args.checker).exists():
        raise SystemExit(f"{args.checker} is missing: build check_lrup_parity (the repository README)")
    arm = measure.load_arms()[ARM]
    limits = measure.Limits(timeout=args.proof_timeout, mem_mb=args.mem_mb, jobs=args.jobs,
                            order_seed=args.order_seed, perf=args.perf, scratch=Path(args.scratch))
    print(f"{len(done)} proof runs recorded, {len(todo)} UNSAT answers to certify, {args.jobs} jobs", flush=True)

    def execute(r: dict) -> dict:
        line = lines[r["line"]]
        xnf = G.write_xnf(line, r["n"], r["m"], r["seed"], G.INSTANCES / "threshold" / line.name)
        if measure.sha256(xnf) != r["xnf_sha256"]:
            raise SystemExit(f"{xnf} is not the file its run read (sha256)")
        rec = measure.proof_run(ARM, arm, xnf.relative_to(measure.HERE), limits, proof_dir=Path(args.proof_dir),
                                checker=Path(args.checker), check_timeout=args.check_timeout)
        return {key: r[key] for key in ("line", "n", "m", "seed", "python", "arm", "family", "name", "xnf_sha256")} | rec

    failures = []

    def record(r: dict, rec: dict) -> str:
        measure.append_jsonl(proofs, rec)
        why = measure.proof_failure(rec)
        if why:
            failures.append(f"{rec['line']} n={rec['n']} m={rec['m']} seed={rec['seed']}: {why}")
            print(f"PROOF FAILURE {failures[-1]}", flush=True)
        return (f"{rec['line']} n={rec['n']} m={rec['m']} seed={rec['seed']} proof {rec['check']}, "
                f"{rec['proof_lines']} lines, {rec['wall_s']} s + check {rec['check_wall_s']} s")

    measure.run_flat(todo, execute, record, jobs=args.jobs, order_seed=args.order_seed)
    recs = read_records(proofs).values()
    bad = [r for r in recs if measure.proof_failure(r) or r["check"] != "verified"]
    print(f"{len(recs)} proof runs recorded, {sum(r['check'] == 'verified' for r in recs)} verified, "
          f"{len(bad)} not verified" + "".join(f"\n  {r['line']} n={r['n']} m={r['m']} seed={r['seed']}: "
                                               f"{r['check']}" for r in bad))
    return 1 if bad else 0


# ---------------------------------------------------------------------------
# fit
# ---------------------------------------------------------------------------


def fit_logistic(points: list[tuple[int, int, int]]) -> tuple[float, float, float, str]:
    """(m50, its standard error, the 90%-to-10% width, note) of P(SAT) = 1 / (1 + e^(s (m - m50))).

    `points` are (m, SAT runs, runs with a verdict). Where no UNSAT lies below any SAT, the
    maximum-likelihood fit does not exist: m50 is then the midpoint between the largest m
    below every UNSAT and the smallest m above every SAT, its error half their distance,
    and the note says "separated".
    """
    points = [p for p in points if p[2]]
    max_sat = max((m for m, s, _ in points if s), default=None)
    min_unsat = min((m for m, s, t in points if s < t), default=None)
    if max_sat is None or min_unsat is None:
        raise ValueError("every run has one verdict: widen the range")
    if min_unsat >= max_sat:
        low = max((m for m, *_ in points if m < min_unsat), default=min_unsat)
        high = min((m for m, *_ in points if m > max_sat), default=max_sat)
        return (low + high) / 2, (high - low) / 2, 0.0, "separated"
    mbar = sum(m * t for m, _, t in points) / sum(t for *_, t in points)

    def terms(b0: float, b1: float) -> tuple[float, float, float, float, float, float]:
        """Log-likelihood, gradient and negated Hessian at logit P(SAT) = b0 + b1 (m - mbar)."""
        ll = g0 = g1 = h00 = h01 = h11 = 0.0
        for m, s, t in points:
            x = m - mbar
            z = max(-700.0, min(700.0, b0 + b1 * x))
            p = 1 / (1 + exp(-z))
            ll += s * z - t * log1p(exp(z)) if z < 30 else s * z - t * (z + log1p(exp(-z)))
            r, w = s - t * p, t * p * (1 - p)
            g0, g1 = g0 + r, g1 + r * x
            h00, h01, h11 = h00 + w, h01 + w * x, h11 + w * x * x
        return ll, g0, g1, h00, h01, h11

    b0 = b1 = 0.0
    ll, g0, g1, h00, h01, h11 = terms(b0, b1)
    for _ in range(200):
        det = h00 * h11 - h01 * h01
        d0, d1 = (h11 * g0 - h01 * g1) / det, (h00 * g1 - h01 * g0) / det
        for _ in range(60):
            new = terms(b0 + d0, b1 + d1)
            if new[0] >= ll - 1e-12:
                break
            d0, d1 = d0 / 2, d1 / 2
        b0, b1 = b0 + d0, b1 + d1
        ll, g0, g1, h00, h01, h11 = new
        if abs(d0) + abs(d1) < 1e-10:
            break
    else:
        raise ValueError("the logistic fit did not converge")
    if b1 >= 0:
        raise ValueError("P(SAT) does not fall with m")
    det = h00 * h11 - h01 * h01
    c00, c01, c11 = h11 / det, -h01 / det, h00 / det
    g = (-1 / b1, b0 / b1**2)
    se = sqrt(g[0] * g[0] * c00 + 2 * g[0] * g[1] * c01 + g[1] * g[1] * c11)
    return mbar - b0 / b1, se, 2 * log(9) / -b1, ""


def fit(lines: dict[str, G.Line]) -> None:
    by_target: dict[tuple[str, int], dict[int, list[dict]]] = {}
    for r in read_records(RUNS_JSONL).values():
        if r["line"] in lines:
            by_target.setdefault((r["line"], r["n"]), {}).setdefault(r["m"], []).append(r)
    fits: dict[str, list[dict]] = {}
    for (name, n), by_m in sorted(by_target.items()):
        points = [(m, sum(r["verdict"] == "SAT" for r in rs), sum(r["verdict"] != "UNKNOWN" for r in rs))
                  for m, rs in sorted(by_m.items())]
        try:
            m50, se, width, note = fit_logistic(points)
        except ValueError as exc:
            raise SystemExit(f"{name} n={n}: {exc}") from None
        unknown = sum(r["verdict"] == "UNKNOWN" for rs in by_m.values() for r in rs)
        fits.setdefault(name, []).append({
            "line": name, "n": n, "m_formula": round(lines[name].formula_m(n), 2), "m50": round(m50, 2),
            "m50_se": round(se, 2), "width_90_10": round(width, 2), "m_low": min(by_m), "m_high": max(by_m),
            "runs": sum(len(rs) for rs in by_m.values()), "unknown": unknown, "note": note})
    rows = []
    for name, fs in fits.items():
        if len(fs) < 2:
            raise SystemExit(f"{name}: m50 at {len(fs)} n; the line needs two or more")
        xs, ys = [f["n"] for f in fs], [f["m50"] for f in fs]
        xbar, ybar = sum(xs) / len(xs), sum(ys) / len(ys)
        a = sum((x - xbar) * (y - ybar) for x, y in zip(xs, ys)) / sum((x - xbar) ** 2 for x in xs)
        b = ybar - a * xbar
        for f in fs:
            f["residual"] = round(f["m50"] - (a * f["n"] + b), 2)
        a_formula, b_formula = lines[name].formula_a_b()
        line = lines[name]
        rows.append({"line": name, "k": line.k, "v": line.v, "a": round(a, 4), "b": round(b, 3),
                     "a_formula": round(a_formula, 4), "b_formula": round(b_formula, 3),
                     "sweep_n": ";".join(map(str, xs)), "max_abs_residual": max(abs(f["residual"]) for f in fs)})
    write_csv(FITS_CSV, [f for fs in fits.values() for f in fs])
    old = []
    if G.THRESHOLDS_CSV.exists():
        with G.THRESHOLDS_CSV.open() as f:
            old = [r for r in csv.DictReader(f) if r["line"] not in fits]
    write_csv(G.THRESHOLDS_CSV, sorted(old + rows, key=lambda r: list(G.load_lines()).index(r["line"])))
    for r in rows:
        print(f"{r['line']}: m*(n) = {r['a']} n + {r['b']} (formula {r['a_formula']} n + {r['b_formula']}), "
              f"largest residual {r['max_abs_residual']}")
    print(f"wrote {FITS_CSV} and {G.THRESHOLDS_CSV}")


def write_csv(path: Path, rows: list[dict]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0]))
        w.writeheader()
        w.writerows(rows)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("step", choices=("probe", "run", "fit", "certify"))
    ap.add_argument("--lines", default="",
                    help="comma-separated lines of lines.csv; default probe: the sparse ones, run and fit: all")
    ap.add_argument("--sparse-ratio", type=float, default=0.95,
                    help="a sparse line's starting clause count, as a share of the formula's (default 0.95)")
    ap.add_argument("--n-from", type=int, default=10, help="probe: the first n (default 10)")
    ap.add_argument("--n-limit", type=int, default=3000, help="probe: the largest n (default 3000)")
    ap.add_argument("--per-n", type=int, default=5, help="probe: instances per n (default 5)")
    ap.add_argument("--target-s", type=float, default=1.0, help="probe: the median time to reach (default 1 s)")
    ap.add_argument("--per-m", type=int, default=30, help="run: instances per (n, m) (default 30)")
    ap.add_argument("--order-seed", type=int, default=0, help="the shuffle of each round's runs")
    measure.add_run_options(ap, jobs=8, scratch="paper-random-threshold")
    measure.add_proof_options(ap, scratch="paper-random-proofs")
    ap.set_defaults(timeout=None)
    args = ap.parse_args()
    args.timeout = args.timeout or (60 if args.step == "probe" else 600)
    lines = G.load_lines(args.lines)
    if args.step == "probe":
        if not args.lines:
            lines = {name: line for name, line in lines.items() if line.v}
        probe(lines, args)
    elif args.step == "run":
        run(lines, args)
    elif args.step == "certify":
        return certify(args)
    else:
        fit(lines)
    return 0


if __name__ == "__main__":
    sys.exit(main())
