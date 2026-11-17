"""The one measured run, the two pools that schedule runs, and the checks of models and proofs.

Every experiment's `run.py` measures through `run_one`:

    perf stat -x, -e instructions:u -- timeout -k 10 T prlimit --as=M <arm's command>

run from this directory, with the instance given by its path relative to it. A record holds
the verdict, wall time, user-space instructions, peak resident set (from `wait4`), Xorcle's
conflict count where the arm writes a stats file, and the last lines of the solver's output.
A SAT answer's `v` lines are saved, gzipped, for `check_model`.

The pools run each ready run in its own thread, unpinned, so every arm sees the same machine
load: `run_flat` takes a fixed list of runs in one seeded shuffle, and `run_chains` takes
chains whose next runs depend on their recorded ones, drawing among the ready runs of all
chains at random.

A proof run (`proof_run`) repeats a timed Xorcle run with `--proof`; the search is
deterministic, so it is the same search. Its binary LRUP(⊕) proof is counted, checked by
`check_lrup_parity` and deleted.
"""

from __future__ import annotations

import csv
import datetime
import gzip
import hashlib
import json
import os
import random
import re
import subprocess
import threading
import time
from collections.abc import Callable, Hashable, Iterable
from pathlib import Path

HERE = Path(__file__).resolve().parent
SOLVERS_JSON = HERE / "solvers.json"
RESULTS = HERE / "results"
MEM_OUT = re.compile(r"bad_alloc|out of memory|Cannot allocate|memory", re.IGNORECASE)


def load_arms() -> dict[str, dict]:
    """The arms of `solvers.json`: each a command line (`{instance}`, `{stats}`, `{seed}`) and verdict patterns."""
    return json.loads(SOLVERS_JSON.read_text())["arms"]


def takes_seed(arm: dict) -> bool:
    return any("{seed}" in part for part in arm["cmd"])


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def read_jsonl(path: Path) -> list[dict]:
    if not path.exists():
        return []
    return [json.loads(line) for line in path.read_text().splitlines() if line]


def append_jsonl(path: Path, rec: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a") as f:
        f.write(json.dumps(rec) + "\n")


# ---------------------------------------------------------------------------
# The measured run
# ---------------------------------------------------------------------------


class Limits:
    """The conditions of a run: limits, the pool's job count and order seed, and where its files go."""

    def __init__(self, *, timeout: int = 300, mem_mb: int = 2048, jobs: int, order_seed: int, perf: str,
                 scratch: Path) -> None:
        self.timeout, self.mem_mb, self.jobs, self.order_seed = timeout, mem_mb, jobs, order_seed
        self.perf, self.scratch = perf, Path(scratch)
        for d in ("perf", "stats", "logs"):
            (self.scratch / d).mkdir(parents=True, exist_ok=True)


def run_one(name: str, arm: dict, instance: Path, limits: Limits, *, solver_seed: int = 0,
            model: Path | None = None, extra: Iterable[str] = ()) -> dict:
    """Run arm `name` once on `instance` and return its record.

    `instance` is relative to this directory. `solver_seed` replaces `{seed}` in the arm's
    command; an arm that takes no seed runs without one and records 0. `extra` options go before
    the instance. A SAT answer's model is written to `model`, if given.
    """
    tag = f"{name}.{threading.get_ident()}"
    perf_out, stats_out = limits.scratch / "perf" / f"{tag}.csv", limits.scratch / "stats" / f"{tag}.csv"
    log_out, err_out = limits.scratch / "logs" / f"{tag}.out", limits.scratch / "logs" / f"{tag}.err"
    for p in (perf_out, stats_out):
        p.unlink(missing_ok=True)
    seed = solver_seed if takes_seed(arm) else 0
    cmd = []
    for part in arm["cmd"]:
        if part == "{instance}":
            cmd.extend(extra)
        cmd.append(part.replace("{instance}", str(instance)).replace("{stats}", str(stats_out))
                   .replace("{seed}", str(seed)))
    full = [limits.perf, "stat", "-x,", "-e", "instructions:u", "-o", str(perf_out), "--",
            "timeout", "-k", "10", str(limits.timeout), "prlimit", f"--as={limits.mem_mb * 1024 * 1024}", *cmd]
    started = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    with log_out.open("wb") as fo, err_out.open("wb") as fe:
        t0 = time.monotonic()
        proc = subprocess.Popen(full, stdout=fo, stderr=fe, cwd=HERE)
        _, status, ru = os.wait4(proc.pid, 0)
        wall = time.monotonic() - t0
    code = os.waitstatus_to_exitcode(status)
    out = log_out.read_text(errors="replace")
    err = err_out.read_text(errors="replace")
    verdict = "UNKNOWN"
    for v in ("UNSAT", "SAT"):
        if re.search(arm["verdict"][v], out, re.MULTILINE):
            verdict = v
            break
    timed_out = verdict == "UNKNOWN" and (code in (124, 137) or wall >= limits.timeout)
    mem_out = verdict == "UNKNOWN" and not timed_out and MEM_OUT.search(out + err) is not None

    instructions = None
    if perf_out.exists():
        for line in perf_out.read_text().splitlines():
            parts = line.split(",")
            if len(parts) > 2 and parts[2].startswith("instructions") and parts[0].isdigit():
                instructions = int(parts[0])
    conflicts = None
    if "{stats}" in " ".join(arm["cmd"]) and stats_out.exists():
        with stats_out.open() as f:
            recs = list(csv.DictReader(f))
        if recs and recs[-1].get("conflicts", "") != "":
            conflicts = int(float(recs[-1]["conflicts"]))
    if verdict == "SAT" and model is not None:
        model.parent.mkdir(parents=True, exist_ok=True)
        with gzip.open(model, "wt") as f:
            f.write("".join(line + "\n" for line in out.splitlines() if line.startswith("v ")))
    tail = [line for line in out.splitlines() if not line.startswith("v ")][-40:]
    for p in (log_out, err_out, perf_out, stats_out):
        p.unlink(missing_ok=True)
    return {
        "arm": name, "timeout_s": limits.timeout, "mem_mb": limits.mem_mb, "jobs": limits.jobs,
        "order_seed": limits.order_seed, "solver_seed": seed, "started_utc": started, "exit": code,
        "verdict": verdict, "wall_s": round(wall, 3), "timed_out": bool(timed_out), "mem_out": bool(mem_out),
        "instructions": instructions, "conflicts": conflicts, "max_rss_mb": round(ru.ru_maxrss / 1024, 1),
        "stdout_tail": "\n".join(tail), "stderr_tail": "\n".join(err.splitlines()[-20:]),
    }


def outcome(rec: dict) -> str:
    """SAT, UNSAT, timeout, memout or other."""
    if rec["verdict"] != "UNKNOWN":
        return rec["verdict"]
    return "timeout" if rec["timed_out"] else "memout" if rec["mem_out"] else "other"


# ---------------------------------------------------------------------------
# The two pools
# ---------------------------------------------------------------------------


def run_flat(runs: list, execute: Callable[[object], dict], record: Callable[[object, dict], str], *, jobs: int,
             order_seed: int) -> None:
    """Run every run of `runs` once, in the order `random.Random(order_seed).shuffle` gives, `jobs` at a time.

    `execute(run)` returns a record, outside the lock; `record(run, rec)` stores it and returns a
    one-line summary, under the lock.
    """
    order = list(runs)
    random.Random(order_seed).shuffle(order)
    lock = threading.Lock()
    it = iter(order)
    done = [0]
    t0 = time.monotonic()

    def worker() -> None:
        while True:
            with lock:
                run = next(it, None)
            if run is None:
                return
            rec = execute(run)
            with lock:
                done[0] += 1
                print(f"[{done[0]}/{len(order)}] {record(run, rec)}", flush=True)

    threads = [threading.Thread(target=worker) for _ in range(jobs)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    print(f"finished in {(time.monotonic() - t0) / 3600:.2f} h", flush=True)


def run_chains(chains: Iterable[Hashable], advance: Callable[[Hashable], tuple[str, list[tuple]]],
               execute: Callable[[tuple], dict], record: Callable[[tuple, dict], str], *, jobs: int,
               order_seed: int) -> dict[Hashable, str]:
    """Run every chain until none has a run ready; returns each chain's final state.

    A chain's next runs depend on its recorded ones. `advance(chain)` returns the chain's state,
    one line of text, and its ready runs: those whose turn has come and that are not recorded.
    A run is a tuple whose first element is its chain. Whenever a job is free it takes one of the
    ready runs of all chains, uniformly at random with `random.Random(order_seed)`. `advance` and
    `record` run under the lock, `execute` outside it.
    """
    cond = threading.Condition()
    rng = random.Random(order_seed)
    ready: list[tuple] = []
    queued: set[tuple] = set()
    state: dict[Hashable, str] = {}
    running, finished = [0], [0]
    errors: list[BaseException] = []
    t0 = time.monotonic()

    def refresh(chain: Hashable) -> None:
        state[chain], runs = advance(chain)
        for run in runs:
            if run not in queued:
                queued.add(run)
                ready.append(run)

    with cond:
        for chain in chains:
            refresh(chain)
    print(f"{len(state)} chains, {len(ready)} runs ready, {jobs} jobs, order seed {order_seed}", flush=True)

    def worker() -> None:
        while True:
            with cond:
                while not ready and running[0] and not errors:
                    cond.wait()
                if not ready or errors:
                    cond.notify_all()
                    return
                run = ready.pop(rng.randrange(len(ready)))
                running[0] += 1
            try:
                rec = execute(run)
            except BaseException as exc:
                with cond:
                    errors.append(exc)
                    running[0] -= 1
                    cond.notify_all()
                raise
            with cond:
                last = record(run, rec)
                queued.discard(run)
                running[0] -= 1
                finished[0] += 1
                before = state[run[0]]
                refresh(run[0])
                changed = state[run[0]] != before
                print(f"[{finished[0]}] {last}" + (f"; chain {state[run[0]]}" if changed else ""), flush=True)
                cond.notify_all()

    threads = [threading.Thread(target=worker) for _ in range(jobs)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    if errors:
        raise SystemExit(f"stopped: a run raised {errors[0]!r}; the runs recorded so far are kept")
    print(f"finished in {(time.monotonic() - t0) / 3600:.2f} h", flush=True)
    return state


# ---------------------------------------------------------------------------
# Model checks
# ---------------------------------------------------------------------------


def parse_formula(path: Path) -> list[list[list[int]]]:
    """The clauses of a CNF or XNF file.

    A clause is a list of equations, an equation a list of signed literals whose XOR must be
    true, and a clause holds if any of its equations does.
    """
    out = []
    for line in path.read_text().splitlines():
        s = line.strip()
        if not s or s[0] in "cp%":
            continue
        toks = s.split()
        if toks[-1] != "0":
            raise ValueError(f"{path}: unterminated line {s!r}")
        out.append([[int(t) for t in tok.split("+")] for tok in toks[:-1]])
    return out


def model_values(model_text: str, n: int | None = None) -> dict[int, bool]:
    """The assignment of a model's `v` lines, only its variables 1..n if `n` is given."""
    value: dict[int, bool] = {}
    for line in model_text.splitlines():
        if line.startswith("v "):
            for t in line[2:].split():
                lit = int(t)
                if lit and (n is None or abs(lit) <= n):
                    value[abs(lit)] = lit > 0
    return value


def check_model(path: Path, value: dict[int, bool]) -> str:
    """'ok', or why the assignment falsifies the file; an unassigned variable is false."""
    if not value:
        return "no model printed"
    for idx, equations in enumerate(parse_formula(path)):
        if not any(sum(value.get(abs(lit), False) ^ (lit < 0) for lit in eq) % 2 == 1 for eq in equations):
            return f"constraint {idx} falsified"
    return "ok"


def read_model(path: Path) -> str | None:
    if not path.is_file():
        return None
    with gzip.open(path, "rt") as f:
        return f.read()


# ---------------------------------------------------------------------------
# Proof runs
# ---------------------------------------------------------------------------

CHUNK = 1 << 24


def count_lines(path: Path) -> tuple[int, int]:
    """(learned lines, deletion lines) of a binary LRUP(⊕) proof.

    Every number is LEB128, whose bytes are 0 only in the number 0, and a 0 ends a learned
    clause's rows, its reasons and a deletion's ids: so a learned line is two segments between
    0 bytes, the first starting with `a`, and a deletion line one, starting with `d`. The empty
    clause's line counts as a learned line.
    """
    learned = deleted = 0
    reasons_next = False
    at_start = True
    with path.open("rb") as f:
        while chunk := f.read(CHUNK):
            pos = 0
            while pos < len(chunk):
                if at_start:
                    if reasons_next:
                        reasons_next = False
                    elif chunk[pos] == 0x61:
                        learned += 1
                        reasons_next = True
                    elif chunk[pos] == 0x64:
                        deleted += 1
                    else:
                        raise ValueError(f"{path}: a line starts with byte {chunk[pos]:#x}, not a or d")
                    at_start = False
                end = chunk.find(b"\0", pos)
                if end < 0:
                    break
                pos = end + 1
                at_start = True
    if not at_start or reasons_next:
        raise ValueError(f"{path}: the last line is incomplete")
    return learned, deleted


def proof_run(name: str, arm: dict, instance: Path, limits: Limits, *, proof_dir: Path, checker: Path,
              check_timeout: int) -> dict:
    """The record of a proof run: `run_one` with `--proof`, then the proof's counts and its check.

    `limits.timeout` is the proof run's own limit; the checker reads `instance` too.
    """
    proof_dir.mkdir(parents=True, exist_ok=True)
    proof = proof_dir / f"{name}.{threading.get_ident()}.lrup"
    proof.unlink(missing_ok=True)
    try:
        rec = run_one(name, arm, instance, limits, extra=[f"--proof={proof}"])
        rec.update(proof_bytes=None, proof_lines=None, proof_deletion_lines=None, count_error=None, check=None,
                   check_learned=None, check_wall_s=None, check_output="")
        if rec["verdict"] != "UNSAT" or not proof.exists():
            rec["check"] = "not run"
            return rec
        rec["proof_bytes"] = proof.stat().st_size
        try:
            rec["proof_lines"], rec["proof_deletion_lines"] = count_lines(proof)
        except ValueError as exc:
            rec["count_error"] = str(exc)
        t0 = time.monotonic()
        try:
            done = subprocess.run([str(checker), str(HERE / instance), str(proof)], capture_output=True, text=True,
                                  check=False, timeout=check_timeout)
            rec["check"] = "verified" if done.returncode == 0 else "failed"
            rec["check_output"] = (done.stdout + done.stderr).strip()[-2000:]
            learned = re.search(r"learned=(\d+)", done.stdout)
            rec["check_learned"] = int(learned.group(1)) if learned else None
        except subprocess.TimeoutExpired:
            rec["check"] = "timeout"
        rec["check_wall_s"] = round(time.monotonic() - t0, 3)
        return rec
    finally:
        proof.unlink(missing_ok=True)


def proof_failure(rec: dict) -> str | None:
    """Why a proof run's record is a failure, or None: a refused proof or two counts that differ."""
    if rec["count_error"]:
        return f"the proof could not be read: {rec['count_error']}"
    if rec["check"] == "failed":
        return f"check_lrup_parity refused the proof: {rec['check_output'][-300:]}"
    if rec["check"] == "verified" and rec["check_learned"] != rec["proof_lines"]:
        return f"{rec['proof_lines']} learned lines counted, {rec['check_learned']} by check_lrup_parity"
    return None


# ---------------------------------------------------------------------------
# Shared command-line options
# ---------------------------------------------------------------------------


def add_run_options(ap, *, jobs: int, scratch: str) -> None:
    ap.add_argument("--jobs", type=int, default=jobs)
    ap.add_argument("--timeout", type=int, default=300)
    ap.add_argument("--mem-mb", type=int, default=2048)
    ap.add_argument("--perf", default="perf", help="the perf binary (default: perf on PATH)")
    ap.add_argument("--scratch", default=str(Path(os.environ.get("TMPDIR", "/tmp")) / scratch),
                    help="per-run perf, stats and log files, deleted as each run is read")


def add_proof_options(ap, *, scratch: str) -> None:
    ap.add_argument("--proof-dir", default=str(Path(os.environ.get("TMPDIR", "/tmp")) / scratch),
                    help="where proofs are written, checked and deleted")
    ap.add_argument("--checker", default=str(HERE.parent / "build" / "check_lrup_parity"))
    ap.add_argument("--proof-timeout", type=int, default=600, help="seconds per proof run (default 600)")
    ap.add_argument("--check-timeout", type=int, default=1200, help="seconds per proof check (default 1200)")
