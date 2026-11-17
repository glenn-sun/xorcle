#!/usr/bin/env python3
"""Run every solver up each proof-complexity line in n, one chain per (solver, line), all chains in one pool.

    run.py [--jobs 32] [--timeout 300] [--mem-mb 2048] [--arms A,B] [--lines L,...] [--perf PATH]
           [--scratch DIR] [--proof-dir DIR] [--checker PATH] [--proof-timeout 600] [--check-timeout 1200]

A chain runs its solver once per n of its line (`lines.csv`, `generate.py`), upwards, at
solver seed 0, on the shuffled `.cnf`, and stops after its second run in a row without a
verdict. The arms are Kissat 4.0.4, CaDiCaL 3.0.1, CryptoMiniSat 5.17.0, Xorricane 0.4.2.11,
Xorcle v0.2 at its defaults, and Xorcle v0.2 plain (phase saving, BVE, vivification and the
implication graph off). The ready runs of all chains are drawn at random by `random.Random(0)`
from one pool of unpinned jobs (`../measure.py`). Each timed run writes no proof and appends
one record to `../results/proof_complexity/<arm>.jsonl`. Instances are written when a run
first needs them.

Every point a Xorcle arm answers UNSAT gets a proof run: the same run with `--proof`, under
`--proof-timeout`; its learned lines are counted, the proof is checked by `check_lrup_parity`
and deleted, and the record goes to `../results/proof_complexity/proofs/<arm>.jsonl`. A
refused proof is reported, and the runner then exits 1. A rerun resumes every chain.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
import measure

import generate as G

RESULTS = measure.RESULTS / "proof_complexity"
ARMS = ("kissat-4.0.4", "cadical-3.0.1", "cms-5.17.0", "xorricane-0.4.2.11", "xorcle-0.2", "xorcle-0.2_plain")
PROOF_ARMS = ("xorcle-0.2", "xorcle-0.2_plain")
#: A chain stops after this many runs in a row without a verdict.
STOP_RUNS = 2
ORDER_SEED = 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--arms", default="", help=f"comma-separated arms; default {','.join(ARMS)}")
    ap.add_argument("--lines", default="", help="comma-separated lines of lines.csv; default all")
    measure.add_run_options(ap, jobs=32, scratch="paper-pc-runs")
    measure.add_proof_options(ap, scratch="paper-pc-proofs")
    args = ap.parse_args()

    arm_names = args.arms.split(",") if args.arms else list(ARMS)
    unknown = [a for a in arm_names if a not in ARMS]
    if unknown:
        raise SystemExit(f"unknown arms: {', '.join(unknown)}; known: {', '.join(ARMS)}")
    if any(a in PROOF_ARMS for a in arm_names) and not Path(args.checker).exists():
        raise SystemExit(f"{args.checker} is missing: build check_lrup_parity (the repository README)")
    arms = measure.load_arms()
    lines = G.load_lines()
    if args.lines:
        lines = {name: lines[name] for name in args.lines.split(",")}
    G.versions()
    limits = measure.Limits(timeout=args.timeout, mem_mb=args.mem_mb, jobs=args.jobs, order_seed=ORDER_SEED,
                            perf=args.perf, scratch=Path(args.scratch))
    proof_limits = measure.Limits(timeout=args.proof_timeout, mem_mb=args.mem_mb, jobs=args.jobs,
                                  order_seed=ORDER_SEED, perf=args.perf, scratch=Path(args.scratch))

    def read(path: Path) -> dict[tuple[str, int], dict]:
        return {(r["line"], r["n"]): r for r in measure.read_jsonl(path)}

    done = {a: read(RESULTS / f"{a}.jsonl") for a in arm_names}
    proofs_done = {a: read(RESULTS / "proofs" / f"{a}.jsonl") for a in arm_names if a in PROOF_ARMS}

    def advance(chain: tuple[str, str]) -> tuple[str, list[tuple]]:
        a, name = chain
        failed, proof_runs = 0, []
        for n in lines[name].ns():
            rec = done[a].get((name, n))
            if rec is None:
                return f"running n={n}", [(chain, n), *proof_runs]
            if a in PROOF_ARMS and rec["verdict"] == "UNSAT" and (name, n) not in proofs_done[a]:
                proof_runs.append((chain, n, "proof"))
            failed = failed + 1 if rec["verdict"] == "UNKNOWN" else 0
            if failed == STOP_RUNS:
                return f"stopped after n={n} ({STOP_RUNS} runs in a row without a verdict)", proof_runs
        return f"reached n_max={lines[name].n_max}", proof_runs

    def execute(run: tuple) -> dict:
        (a, name), n, *proof = run
        line = lines[name]
        row = G.point(line, n)
        instance = Path("proof_complexity") / row["dir"] / f"{row['name']}.cnf"
        if proof:
            rec = measure.proof_run(a, arms[a], instance, proof_limits, proof_dir=Path(args.proof_dir),
                                    checker=Path(args.checker), check_timeout=args.check_timeout)
        else:
            rec = measure.run_one(a, arms[a], instance, limits)
        return {"line": name, "family": line.family, "k": line.k, "n": n, "seed": line.seed, "order": "shuffle",
                "shuffle_seed": G.SHUFFLE_SEED, "cnf_sha256": row["cnf_sha256"], "arm": a, "name": row["name"], **rec}

    failures: list[str] = []

    def record(run: tuple, rec: dict) -> str:
        (a, name), n, *proof = run
        measure.append_jsonl(RESULTS / "proofs" / f"{a}.jsonl" if proof else RESULTS / f"{a}.jsonl", rec)
        tag = f"{a} {name} n={n}"
        if proof:
            proofs_done[a][(name, n)] = rec
            why = measure.proof_failure(rec)
            if why:
                failures.append(f"{tag}: {why}")
                print(f"PROOF FAILURE {failures[-1]}", flush=True)
            return f"{tag} proof {rec['check']}, {rec['proof_lines']} lines, {rec['wall_s']} s + check {rec['check_wall_s']} s"
        done[a][(name, n)] = rec
        if rec["verdict"] == "SAT":
            print(f"WARNING {tag}: SAT on an unsatisfiable family", flush=True)
        return f"{tag} {measure.outcome(rec)} {rec['wall_s']} s"

    chains = [(a, name) for a in arm_names for name in lines]
    print(f"{len(chains)} chains ({len(arm_names)} arms, {len(lines)} lines); {args.timeout} s, {args.mem_mb} MB",
          flush=True)
    state = measure.run_chains(chains, advance, execute, record, jobs=args.jobs, order_seed=ORDER_SEED)
    for chain, s in sorted(state.items()):
        print(f"{' '.join(chain)}: {s}")
    failures = [f"{a} {name} n={n}: {why}" for a, recs in proofs_done.items()
                for (name, n), r in recs.items() if (why := measure.proof_failure(r))]
    print(f"{sum(map(len, proofs_done.values()))} proof runs recorded, {len(failures)} failures"
          + "".join(f"\n  FAILURE {f}" for f in failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
