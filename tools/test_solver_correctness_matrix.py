#!/usr/bin/env python3
from __future__ import annotations

import argparse
import re
import subprocess
import sys
import tempfile
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional, Sequence

sys.path.append(str(Path(__file__).resolve().parent))
from xnf_lib import Clause, Equation, parse_xnf_file


DEFAULT_INPUTS = (
    "sat-comp-tests/3619135ee5865be0a107449fb31defd9-arles_thres20_p10_r7475.cnf",
    "ascon-tests/r2/tmp_jsyfpuo.xnf",
    "tests/bijphp_n_n+1/bijphp_6.cnf",
    "tests/pebbling_3lift_h/pebbling_3lift_150.cnf",
    "tests/random_3xnf_n/random_3xnf_15_01.cnf",
    "tests/tseitin_5_n/tseitin_5_n_80_01.cnf",
)
DECISION_PROCS = ("vsids_unit", "cvsids")
CLAUSE_SELECTORS = ("cyclic", "lt_watchlist", "exact_watchlist")
RESULT_RE = re.compile(r"^Result:\s*(SAT|UNSAT|TIMEOUT)\s*$", re.MULTILINE)
ASSIGNMENT_RE = re.compile(r"^Satisfying assignment:\s*\[(.*?)\]\s*$", re.MULTILINE | re.DOTALL)


@dataclass(frozen=True)
class Task:
    input_path: Path
    decision_proc: str
    clause_selector: str


@dataclass
class ProcRun:
    returncode: Optional[int]
    stdout: str
    stderr: str
    elapsed_wall_s: float
    timed_out: bool
    error_message: str = ""

    @property
    def combined_output(self) -> str:
        if self.stdout and self.stderr:
            return self.stdout + "\n" + self.stderr
        return self.stdout or self.stderr


@dataclass
class RunResult:
    task: Task
    solver_status: str
    validation_status: str
    elapsed_wall_s: float
    message: str = ""


def repo_root() -> Path:
    return Path(__file__).resolve().parents[1]


def default_solver_bin(root: Path) -> Path:
    release = root / "bin" / "main-release"
    if release.exists():
        return release
    debug = root / "bin" / "main"
    if debug.exists():
        return debug
    raise FileNotFoundError("No default solver binary found at bin/main-release or bin/main")


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    root = repo_root()
    parser = argparse.ArgumentParser(
        description="Run the fixed solver correctness matrix over the selected benchmark inputs."
    )
    try:
        default_solver = str(default_solver_bin(root))
    except FileNotFoundError:
        default_solver = ""
    parser.add_argument("--solver-bin", default=default_solver)
    parser.add_argument("--checker", default=str(root / "tools" / "check_drup_plus.py"))
    parser.add_argument("--timeout", type=float, default=5.0)
    parser.add_argument("--jobs", type=int, default=4)
    args = parser.parse_args(argv)

    if args.timeout <= 0:
        parser.error("--timeout must be > 0")
    if args.jobs <= 0:
        parser.error("--jobs must be > 0")
    return args


def run_process(cmd: Sequence[str], timeout_s: Optional[float]) -> ProcRun:
    start = time.perf_counter()
    try:
        cp = subprocess.run(
            list(cmd),
            capture_output=True,
            text=True,
            timeout=timeout_s,
            shell=False,
        )
        return ProcRun(
            returncode=cp.returncode,
            stdout=cp.stdout or "",
            stderr=cp.stderr or "",
            elapsed_wall_s=time.perf_counter() - start,
            timed_out=False,
        )
    except subprocess.TimeoutExpired as exc:
        stdout = exc.stdout or ""
        stderr = exc.stderr or ""
        if isinstance(stdout, bytes):
            stdout = stdout.decode(errors="replace")
        if isinstance(stderr, bytes):
            stderr = stderr.decode(errors="replace")
        return ProcRun(
            returncode=None,
            stdout=stdout,
            stderr=stderr,
            elapsed_wall_s=time.perf_counter() - start,
            timed_out=True,
            error_message=f"timeout after {timeout_s:.1f}s",
        )
    except OSError as exc:
        return ProcRun(
            returncode=None,
            stdout="",
            stderr="",
            elapsed_wall_s=time.perf_counter() - start,
            timed_out=False,
            error_message=str(exc),
        )


def parse_solver_status(output: str) -> Optional[str]:
    match = RESULT_RE.search(output)
    if match is None:
        return None
    return match.group(1)


def parse_assignment(output: str) -> Optional[List[int]]:
    match = ASSIGNMENT_RE.search(output)
    if match is None:
        return None
    raw = match.group(1).strip()
    if not raw:
        return []
    try:
        return [int(tok) for tok in raw.split()]
    except ValueError:
        return None


def evaluate_equation(eq: Equation, assignment: Sequence[int]) -> bool:
    parity = 0
    for var in eq.vars:
        parity ^= assignment[var - 1]
    return parity == eq.rhs


def validate_sat_assignment(input_path: Path, assignment: Sequence[int]) -> tuple[bool, str]:
    try:
        nvars, clauses = parse_xnf_file(str(input_path))
    except Exception as exc:
        return False, f"failed to parse input for SAT validation: {exc}"

    if len(assignment) != nvars:
        return False, f"assignment length {len(assignment)} did not match header variable count {nvars}"
    if any(bit not in (0, 1) for bit in assignment):
        return False, "assignment contained non-binary values"

    for clause_idx, clause in enumerate(clauses, start=1):
        if clause_satisfied(clause, assignment):
            continue
        return False, f"assignment did not satisfy clause {clause_idx}"
    return True, "assignment valid"


def clause_satisfied(clause: Clause, assignment: Sequence[int]) -> bool:
    return any(evaluate_equation(eq, assignment) for eq in clause.equations)


def validate_unsat_proof(checker: Path, input_path: Path, proof_path: Path, timeout_s: float) -> tuple[bool, str]:
    proc = run_process([sys.executable, str(checker), str(input_path), str(proof_path)], timeout_s=timeout_s)
    if proc.timed_out:
        return False, f"DRUP checker timed out: {proc.error_message}"
    if proc.error_message:
        return False, f"DRUP checker error: {proc.error_message}"
    if proc.returncode != 0:
        detail = proc.combined_output.strip()
        if detail:
            detail = detail.splitlines()[-1]
        return False, f"DRUP checker failed (rc={proc.returncode}): {detail}"
    return True, "proof valid"


def run_task(task: Task, solver_bin: Path, checker: Path, timeout_s: float) -> RunResult:
    try:
        with tempfile.NamedTemporaryFile(prefix="solver-proof-", suffix=".drup", delete=True) as proof_file:
            cmd = [
                str(solver_bin),
                "-d",
                task.decision_proc,
                "-c",
                task.clause_selector,
                str(task.input_path),
                proof_file.name,
            ]
            proc = run_process(cmd, timeout_s=timeout_s)

            if proc.timed_out:
                return RunResult(task, "TIMEOUT", "FAIL", proc.elapsed_wall_s, proc.error_message)
            if proc.error_message:
                return RunResult(task, "ERROR", "FAIL", proc.elapsed_wall_s, proc.error_message)
            if proc.returncode != 0:
                detail = proc.combined_output.strip()
                if detail:
                    detail = detail.splitlines()[-1]
                return RunResult(
                    task,
                    "ERROR",
                    "FAIL",
                    proc.elapsed_wall_s,
                    f"solver failed (rc={proc.returncode}): {detail}",
                )

            solver_status = parse_solver_status(proc.combined_output)
            if solver_status is None:
                return RunResult(task, "ERROR", "FAIL", proc.elapsed_wall_s, "could not parse solver result")
            if solver_status == "TIMEOUT":
                return RunResult(task, solver_status, "FAIL", proc.elapsed_wall_s, "solver reported TIMEOUT")

            if solver_status == "SAT":
                assignment = parse_assignment(proc.combined_output)
                if assignment is None:
                    return RunResult(
                        task,
                        solver_status,
                        "FAIL",
                        proc.elapsed_wall_s,
                        "could not parse satisfying assignment",
                    )
                ok, message = validate_sat_assignment(task.input_path, assignment)
                return RunResult(task, solver_status, "PASS" if ok else "FAIL", proc.elapsed_wall_s, message)

            ok, message = validate_unsat_proof(checker, task.input_path, Path(proof_file.name), timeout_s)
            return RunResult(task, solver_status, "PASS" if ok else "FAIL", proc.elapsed_wall_s, message)
    except Exception as exc:
        return RunResult(task, "ERROR", "FAIL", 0.0, f"unexpected exception: {exc}")


def format_result(result: RunResult, root: Path) -> str:
    rel = result.task.input_path.relative_to(root)
    return (
        f"{str(rel):68} "
        f"{result.task.decision_proc:10} "
        f"{result.task.clause_selector:15} "
        f"{result.solver_status:7} "
        f"{result.validation_status:4} "
        f"{result.elapsed_wall_s:7.3f}s "
        f"{result.message}"
    )


def check_consistency(results: Sequence[RunResult]) -> List[str]:
    by_file: Dict[Path, set[str]] = {}
    for result in results:
        if result.validation_status != "PASS":
            continue
        if result.solver_status not in {"SAT", "UNSAT"}:
            continue
        by_file.setdefault(result.task.input_path, set()).add(result.solver_status)

    failures: List[str] = []
    for input_path, statuses in sorted(by_file.items()):
        if len(statuses) > 1:
            rendered = ", ".join(sorted(statuses))
            failures.append(f"{input_path}: inconsistent validated outcomes across configs ({rendered})")
    return failures


def build_tasks(root: Path) -> List[Task]:
    tasks: List[Task] = []
    for rel_input in DEFAULT_INPUTS:
        input_path = (root / rel_input).resolve()
        if not input_path.exists():
            raise FileNotFoundError(f"Missing input file: {input_path}")
        for decision_proc in DECISION_PROCS:
            for clause_selector in CLAUSE_SELECTORS:
                tasks.append(Task(input_path=input_path, decision_proc=decision_proc, clause_selector=clause_selector))
    return tasks


def main(argv: Sequence[str]) -> int:
    args = parse_args(argv)
    root = repo_root()
    solver_bin = Path(args.solver_bin).resolve()
    checker = Path(args.checker).resolve()

    if not solver_bin.exists():
        print(f"error: solver binary does not exist: {solver_bin}", file=sys.stderr)
        return 2
    if not checker.exists():
        print(f"error: checker does not exist: {checker}", file=sys.stderr)
        return 2

    try:
        tasks = build_tasks(root)
    except Exception as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    results: List[RunResult] = []
    print(f"Running {len(tasks)} tasks with {args.jobs} workers")
    print(f"Solver:  {solver_bin}")
    print(f"Checker: {checker}")
    print(f"Timeout: {args.timeout:.1f}s")
    print()

    with ThreadPoolExecutor(max_workers=args.jobs) as executor:
        future_to_task = {
            executor.submit(run_task, task, solver_bin, checker, args.timeout): task
            for task in tasks
        }
        for future in as_completed(future_to_task):
            result = future.result()
            results.append(result)
            print(format_result(result, root))

    results.sort(key=lambda r: (str(r.task.input_path), r.task.decision_proc, r.task.clause_selector))

    pass_count = sum(1 for r in results if r.validation_status == "PASS")
    fail_count = len(results) - pass_count
    consistency_failures = check_consistency(results)

    print()
    print("Summary:")
    print(f"  total runs: {len(results)}")
    print(f"  passed: {pass_count}")
    print(f"  failed: {fail_count}")
    if consistency_failures:
        print("  consistency: FAIL")
        for failure in consistency_failures:
            print(f"    {failure}")
    else:
        print("  consistency: PASS")

    return 0 if fail_count == 0 and not consistency_failures else 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
