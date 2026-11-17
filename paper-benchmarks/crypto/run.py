#!/usr/bin/env python3
"""Run the solvers on Danner's 350 cryptographic instances, all (arm, instance) pairs in one shuffled pool.

    run.py --seed S [--arms all|seeded|unseeded|A,B] [--solved-at S,S] [--jobs 32] [--timeout 300]
           [--mem-mb 2048] [--perf PATH] [--scratch DIR]
    run.py --check-models [--jobs 4]

`--seed S` is the pool's order seed and the solver seed of every arm that takes one; an arm
that takes none (Xorricane, 2-Xornado) runs without one and records solver seed 0. The pairs
are shuffled once by `random.Random(S)` and run from one pool of `--jobs` unpinned jobs
(`../measure.py`). `--solved-at` keeps only the pairs some run at those seeds solved. Each run
appends one record to `../results/crypto/<arm>.jsonl`, with the instance's family, name and
`.xnf` sha256; a rerun skips the pairs recorded at the same seed. A SAT answer's model goes to
`../results/crypto/models/`.

`--check-models` checks every recorded SAT answer's model against the file the solver read
and against the `.xnf`: each encoding keeps the `.xnf`'s variables 1..n and adds its own after
them. It also reports any instance answered SAT by one run and UNSAT by another.
"""

from __future__ import annotations

import argparse
import csv
import sys
from collections import Counter, defaultdict
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
import measure

INSTANCES_CSV = HERE / "instances.csv"
RESULTS = measure.RESULTS / "crypto"
#: The 8 solvers of the comparison, then Xorcle v0.2 with one default feature off.
ARMS = ["kissat-4.0.4", "cadical-3.0.1", "cms-5.17.0", "cms-5.13.0", "xorricane-0.4.2.11", "2xornado-1.0.2",
        "xorcle-0.1", "xorcle-0.2", "xorcle-0.2_selection=cyclic", "xorcle-0.2_deletion=false",
        "xorcle-0.2_csids-intersection-depth=1", "xorcle-0.2_phase-saving=none",
        "xorcle-0.2_implication-graph=false", "xorcle-0.2_compact=false", "xorcle-0.2_minimize=false",
        "xorcle-0.2_vivify=false", "xorcle-0.2_otf-subsume=false", "xorcle-0.2_bve=false"]
#: The file each solver reads, as Danner distributes it: CryptoMiniSat reads the `.cnf`, as in Soos's xnf-tests.
READS = {"kissat-4.0.4": ".cnf", "cadical-3.0.1": ".cnf", "cms-5.17.0": ".cnf", "cms-5.13.0": ".cnf",
         "xorricane-0.4.2.11": ".xnf", "2xornado-1.0.2": ".2xnf", "xorcle-0.1": ".xnf", "xorcle-0.2": ".xnf"}


def reads(arm: str) -> str:
    return READS[measure.load_arms()[arm]["solver"]]


def load_instances() -> list[dict]:
    with INSTANCES_CSV.open() as f:
        return sorted(csv.DictReader(f), key=lambda r: (r["family"], r["name"]))


def instance_path(row: dict, ext: str) -> Path:
    """The path from `..`, which measure.py runs from."""
    return Path("crypto") / row["dir"] / (row["name"] + ext)


def model_path(rec: dict) -> Path:
    return RESULTS / "models" / rec["arm"] / str(rec["order_seed"]) / f"{rec['family']}__{rec['name']}.v.gz"


def load_records(arms: list[str]) -> dict[str, list[dict]]:
    return {a: measure.read_jsonl(RESULTS / f"{a}.jsonl") for a in arms}


def solved(rec: dict) -> bool:
    return rec["verdict"] in ("SAT", "UNSAT")


def run(args: argparse.Namespace) -> int:
    all_arms = measure.load_arms()
    if args.arms in ("all", "seeded", "unseeded"):
        names = [a for a in ARMS if args.arms == "all" or measure.takes_seed(all_arms[a]) == (args.arms == "seeded")]
    else:
        names = args.arms.split(",")
        unknown = [a for a in names if a not in ARMS]
        if unknown:
            raise SystemExit(f"unknown arms: {', '.join(unknown)}")
    rows = load_instances()
    records = load_records(names)
    pairs = [(a, r) for a in names for r in rows]
    if args.solved_at:
        seeds = {int(s) for s in args.solved_at.split(",")}
        won = {(a, rec["family"], rec["name"]) for a in names for rec in records[a]
               if rec["order_seed"] in seeds and solved(rec)}
        pairs = [(a, r) for a, r in pairs if (a, r["family"], r["name"]) in won]
    done = {(a, rec["family"], rec["name"]) for a in names for rec in records[a] if rec["order_seed"] == args.seed}
    todo = [(a, r) for a, r in pairs if (a, r["family"], r["name"]) not in done]
    print(f"{len(pairs)} pairs ({len(names)} arms) at seed {args.seed}: {len(pairs) - len(todo)} done, "
          f"{len(todo)} to run; {args.jobs} jobs, {args.timeout} s, {args.mem_mb} MB", flush=True)
    limits = measure.Limits(timeout=args.timeout, mem_mb=args.mem_mb, jobs=args.jobs, order_seed=args.seed,
                            perf=args.perf, scratch=Path(args.scratch))

    def execute(pair: tuple[str, dict]) -> dict:
        a, r = pair
        head = {"arm": a, "family": r["family"], "name": r["name"], "xnf_sha256": r["xnf_sha256"]}
        model = model_path({**head, "order_seed": args.seed})
        return {**head, **measure.run_one(a, all_arms[a], instance_path(r, reads(a)), limits, solver_seed=args.seed,
                                          model=model)}

    def record(pair: tuple[str, dict], rec: dict) -> str:
        measure.append_jsonl(RESULTS / f"{rec['arm']}.jsonl", rec)
        return f"{rec['arm']} {rec['family']}/{rec['name']} {measure.outcome(rec)} {rec['wall_s']} s"

    measure.run_flat(todo, execute, record, jobs=args.jobs, order_seed=args.seed)
    return 0


def check_one(job: tuple[str, str, str]) -> str:
    model, read, xnf = job
    text = measure.read_model(Path(model))
    if text is None:
        return "no model saved"
    value = measure.model_values(text)
    first = measure.check_model(Path(read), value)
    second = measure.check_model(Path(xnf), value) if xnf != read else first
    return "ok" if first == second == "ok" else f"{Path(read).suffix}: {first}; .xnf: {second}"


def check_models(jobs: int) -> int:
    rows = {(r["family"], r["name"]): r for r in load_instances()}
    records = load_records(ARMS)
    verdicts: dict[tuple, set] = defaultdict(set)
    work, keys = [], []
    for a, recs in records.items():
        for rec in recs:
            if solved(rec):
                verdicts[(rec["family"], rec["name"])].add(rec["verdict"])
            if rec["verdict"] == "SAT":
                row = rows[(rec["family"], rec["name"])]
                work.append((str(model_path(rec)), str(measure.HERE / instance_path(row, reads(a))),
                             str(measure.HERE / instance_path(row, ".xnf"))))
                keys.append(rec)
    counts: Counter = Counter()
    with ProcessPoolExecutor(jobs) as pool:
        for rec, res in zip(keys, pool.map(check_one, work, chunksize=8)):
            counts[(rec["arm"], res == "ok")] += 1
            if res != "ok":
                print(f"FAIL {rec['arm']} seed {rec['order_seed']} {rec['family']}/{rec['name']}: {res}", flush=True)
    disagree = [k for k, v in verdicts.items() if len(v) > 1]
    for k in disagree:
        print(f"DISAGREE {k[0]}/{k[1]}: {sorted(verdicts[k])}")
    for a in ARMS:
        print(f"{a}: {counts[(a, True)]} SAT models ok, {counts[(a, False)]} failed")
    bad = sum(v for (_, ok), v in counts.items() if not ok)
    print(f"{sum(counts.values())} SAT models checked, {bad} failed; {len(disagree)} instances with disagreeing verdicts")
    return 1 if bad or disagree else 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--seed", type=int)
    ap.add_argument("--arms", default="all", help="all (default), seeded, unseeded, or comma-separated arms")
    ap.add_argument("--solved-at", default="", help="only the pairs some run at these seeds solved")
    ap.add_argument("--check-models", action="store_true")
    measure.add_run_options(ap, jobs=32, scratch="paper-crypto-runs")
    ap.set_defaults(jobs=None)
    args = ap.parse_args()
    if args.check_models:
        return check_models(args.jobs or 4)
    args.jobs = args.jobs or 32
    if args.seed is None:
        ap.error("--seed is required")
    return run(args)


if __name__ == "__main__":
    sys.exit(main())
