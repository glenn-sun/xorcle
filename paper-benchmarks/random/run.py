#!/usr/bin/env python3
"""Run every solver up each random line in n, one chain per (solver, line), all chains in one pool.

    run.py [--jobs 32] [--timeout 300] [--mem-mb 2048] [--seeds 101-110] [--arms A,B] [--lines L,...]
           [--perf PATH] [--scratch DIR]
           [--certify-unsat [--proof-dir DIR] [--checker PATH] [--proof-timeout 600] [--check-timeout 1200]]
    run.py --check-models [--jobs 4]

A chain starts at its line's `n_start` (`lines.csv`) and runs its solver once, at solver seed
0, on each instance of the batch at that n: the rows of `instances.csv` with one of
`--seeds`, the same files for every solver. When the batch is complete, the chain stops if at
least 80% of its runs ended without a verdict (timed out, out of memory or failed), and
otherwise moves on to n + `n_step`. The ready runs of all chains are drawn at random by
`random.Random(0)` from one pool of unpinned jobs (`../measure.py`). Each run appends one
record to `../results/random/<arm>.jsonl`, with the line, n, m and instance seed; a SAT
answer's model goes to `../results/random/models/`. A rerun resumes every chain.

With `--certify-unsat`, each UNSAT answer of Xorcle v0.2 is followed by a proof run: the same
run with `--proof`, its proof checked by `check_lrup_parity` against the `.xnf` and deleted,
recorded in `../results/random/proofs/xorcle-0.2.jsonl`. A refused proof is reported, and the
runner then exits 1.

`--check-models` checks every SAT model of the scaling runs and of `threshold.py`, restricted
to the variables 1..n that every encoding keeps, against the `.xnf`, which must still have the
sha256 its run recorded; and reports any instance answered SAT by one run and UNSAT by another.
"""

from __future__ import annotations

import argparse
import sys
from collections import Counter, defaultdict
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
import measure

import generate as G

RESULTS = G.RESULTS
#: The file each solver reads.
READS = {"kissat-4.0.4": ".cnf", "cadical-3.0.1": ".cnf", "cms-5.17.0": ".xcnf", "xorricane-0.4.2.11": ".xnf",
         "xorcle-0.2": ".xnf"}
CERTIFIED = "xorcle-0.2"
#: A chain stops after the first n at which at least this share of its runs has no verdict.
STOP_SHARE = 0.8
ORDER_SEED = 0


def run(args: argparse.Namespace) -> int:
    arm_names = args.arms.split(",") if args.arms else list(READS)
    unknown = [a for a in arm_names if a not in READS]
    if unknown:
        raise SystemExit(f"unknown arms: {', '.join(unknown)}; known: {', '.join(READS)}")
    arms = measure.load_arms()
    lines = G.load_lines(args.lines)
    seeds = G.parse_seeds(args.seeds)
    instances = G.read_rows()
    if not instances:
        raise SystemExit(f"{G.INSTANCES_CSV} is missing or empty: run generate.py first")
    limits = measure.Limits(timeout=args.timeout, mem_mb=args.mem_mb, jobs=args.jobs, order_seed=ORDER_SEED,
                            perf=args.perf, scratch=Path(args.scratch))
    proof_limits = measure.Limits(timeout=args.proof_timeout, mem_mb=args.mem_mb, jobs=args.jobs,
                                  order_seed=ORDER_SEED, perf=args.perf, scratch=Path(args.scratch))

    def read(p: Path) -> dict[tuple[str, int, int], dict]:
        out = {}
        for r in measure.read_jsonl(p):
            row = instances.get((r["line"], r["n"], r["seed"]))
            if row is not None and row["xnf_sha256"] != r["xnf_sha256"]:
                raise SystemExit(f"{p}: {r['line']} n={r['n']} seed={r['seed']} ran on another file than instances.csv lists")
            out[(r["line"], r["n"], r["seed"])] = r
        return out

    done = {a: read(RESULTS / f"{a}.jsonl") for a in arm_names}
    certify = args.certify_unsat and CERTIFIED in arm_names
    proofs_done = read(RESULTS / "proofs" / f"{CERTIFIED}.jsonl") if certify else {}
    if certify and not Path(args.checker).exists():
        raise SystemExit(f"{args.checker} is missing: build check_lrup_parity (the repository README)")

    def advance(chain: tuple[str, str]) -> tuple[str, list[tuple]]:
        """Walk the chain up from n_start through its complete batches, to the first incomplete one."""
        a, name = chain
        line = lines[name]
        n = line.n_start
        proof_runs = []
        while True:
            if any((name, n, s) not in instances for s in seeds):
                return f"no instances at n={n}: raise n_max in lines.csv and run generate.py", proof_runs
            missing = [(chain, n, s) for s in seeds if (name, n, s) not in done[a]]
            if certify and a == CERTIFIED:
                proof_runs += [(chain, n, s, "proof") for s in seeds if (name, n, s) not in proofs_done
                               and done[a].get((name, n, s), {}).get("verdict") == "UNSAT"]
            if missing:
                return f"running n={n}", missing + proof_runs
            unsolved = sum(done[a][(name, n, s)]["verdict"] == "UNKNOWN" for s in seeds)
            if unsolved >= STOP_SHARE * len(seeds):
                return f"stopped after n={n} ({unsolved}/{len(seeds)} without a verdict)", proof_runs
            n += line.n_step

    def execute(run: tuple) -> dict:
        (a, name), n, seed, *proof = run
        row = instances[(name, n, seed)]
        head = {"line": name, "n": n, "m": int(row["m"]), "seed": seed, "arm": a, "family": name,
                "name": row["name"], "xnf_sha256": row["xnf_sha256"]}
        stem = Path("random") / row["dir"] / row["name"]
        if proof:
            return {**head, **measure.proof_run(a, arms[a], stem.with_suffix(".xnf"), proof_limits,
                                                proof_dir=Path(args.proof_dir), checker=Path(args.checker),
                                                check_timeout=args.check_timeout)}
        model = RESULTS / "models" / a / f"{name}__{row['name']}.v.gz"
        return {**head, **measure.run_one(a, arms[a], stem.with_suffix(READS[a]), limits, model=model)}

    failures = []

    def record(run: tuple, rec: dict) -> str:
        (a, name), n, seed, *proof = run
        measure.append_jsonl(RESULTS / "proofs" / f"{a}.jsonl" if proof else RESULTS / f"{a}.jsonl", rec)
        if proof:
            proofs_done[(name, n, seed)] = rec
            why = measure.proof_failure(rec)
            if why:
                failures.append(f"{a} {name} n={n} seed={seed}: {why}")
                print(f"PROOF FAILURE {failures[-1]}", flush=True)
            return (f"{a} {name} n={n} seed={seed} proof {rec['check']}, {rec['proof_lines']} lines, "
                    f"{rec['wall_s']} s + check {rec['check_wall_s']} s")
        done[a][(name, n, seed)] = rec
        return f"{a} {name} n={n} seed={seed} {measure.outcome(rec)} {rec['wall_s']} s"

    print(f"{len(arm_names)} solvers x {len(lines)} lines, seeds {args.seeds}; {args.timeout} s, {args.mem_mb} MB"
          + (f"; {CERTIFIED}'s UNSAT answers certified" if certify else ""), flush=True)
    state = measure.run_chains([(a, name) for a in arm_names for name in lines], advance, execute, record,
                               jobs=args.jobs, order_seed=ORDER_SEED)
    for (a, name), s in sorted(state.items()):
        print(f"{a} {name}: {s}")
    if certify:
        failures = [f"{CERTIFIED} {k}: {why}" for k, r in proofs_done.items() if (why := measure.proof_failure(r))]
        print(f"{len(proofs_done)} proof runs recorded, {len(failures)} failures"
              + "".join(f"\n  FAILURE {f}" for f in failures))
    return 1 if failures else 0


def check_one(job: tuple[str, str, int, str]) -> str:
    model, xnf, n, sha = job
    if not Path(xnf).exists():
        return f"{xnf} is missing"
    if measure.sha256(Path(xnf)) != sha:
        return f"{xnf} is not the file the run read (sha256)"
    text = measure.read_model(Path(model))
    if text is None:
        return "no model saved"
    return measure.check_model(Path(xnf), measure.model_values(text, n))


def check_models(jobs: int) -> int:
    rows = {(r["line"], r["name"]): r for r in G.read_rows().values()}
    recs = []
    for p in sorted(RESULTS.glob("*.jsonl")):
        for r in measure.read_jsonl(p):
            recs.append((r, RESULTS / "models", HERE / rows[(r["line"], r["name"])]["dir"] / f"{r['name']}.xnf"))
    for name in ("probe.jsonl", "runs.jsonl"):
        for r in measure.read_jsonl(RESULTS / "threshold" / name):
            recs.append((r, RESULTS / "threshold" / "models", G.INSTANCES / "threshold" / r["line"] / f"{r['name']}.xnf"))
    verdicts: dict[tuple, dict[str, list[str]]] = defaultdict(lambda: defaultdict(list))
    work, arms = [], []
    for r, models, xnf in recs:
        verdicts[(r["line"], r["xnf_sha256"])][r["verdict"]].append(f"{r['arm']} {r['name']}")
        if r["verdict"] == "SAT":
            work.append((str(models / r["arm"] / f"{r['line']}__{r['name']}.v.gz"), str(xnf), r["n"], r["xnf_sha256"]))
            arms.append(r["arm"])
    counts: Counter = Counter()
    failures = 0
    with ProcessPoolExecutor(jobs) as pool:
        for arm, job, result in zip(arms, work, pool.map(check_one, work, chunksize=64)):
            counts[(arm, result == "ok")] += 1
            if result != "ok":
                failures += 1
                print(f"FAIL {job[0]}: {result}")
    for (line, _), by in sorted(verdicts.items()):
        if by.get("SAT") and by.get("UNSAT"):
            failures += 1
            print(f"DISAGREE {line}: SAT by {', '.join(by['SAT'])}; UNSAT by {', '.join(by['UNSAT'])}")
    for arm in sorted({a for a, _ in counts}):
        print(f"{arm}: {counts[(arm, True)]} models ok, {counts[(arm, False)]} failed")
    print(f"{len(recs)} runs, {len(work)} SAT models, {len(verdicts)} instances; {failures} failure(s)")
    return 1 if failures else 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--seeds", default="101-110", help="each n's batch of instance seeds (default 101-110)")
    ap.add_argument("--arms", default="", help=f"comma-separated arms; default {','.join(READS)}")
    ap.add_argument("--lines", default="", help="comma-separated lines of lines.csv; default all")
    ap.add_argument("--certify-unsat", action="store_true",
                    help=f"also certify each UNSAT answer of {CERTIFIED} by a proof run (off by default)")
    ap.add_argument("--check-models", action="store_true")
    measure.add_run_options(ap, jobs=32, scratch="paper-random-runs")
    measure.add_proof_options(ap, scratch="paper-random-proofs")
    ap.set_defaults(jobs=None)
    args = ap.parse_args()
    if args.check_models:
        return check_models(args.jobs or 4)
    args.jobs = args.jobs or 32
    return run(args)


if __name__ == "__main__":
    sys.exit(main())
