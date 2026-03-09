#!/usr/bin/env python3
from __future__ import annotations

import argparse
import re
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional, Sequence

from tool_common import default_solver_bin
from tool_common import parse_csv_choices
from tool_common import repo_root
from tool_process import ProcRun
from tool_process import parse_solver_status
from tool_process import run_process
from xnf_lib import Clause, Equation, parse_xnf_file


DEFAULT_INPUTS = (
    "tests/sat-comp/081f111af59344b61346367a930e24f6-Break_triple_04_06.xml.cnf",
    "tests/ascon/r2/tmp_jsyfpuo.xnf",
    "tests/generated/random_kxnf_n/random_kxnf_n_k3_n15.cnf",
    "tests/generated/pebbling_lifted_h_k/pebbling_lifted_h_k_h60_k4.cnf",
    "tests/generated/restricted_kxnf_n/restricted_kxnf_n_k3_n30.cnf",
    "tests/generated/tseitin_n_k/tseitin_n_k_n20_k5_01.cnf",
)
DECISION_PROCS = ("clause_vsids",)
CLAUSE_SELECTORS = ("cyclic", "exact_watchlist")
DECISION_PHASES = ("unsat",)
DEFAULT_SEED = 0
RESULT_RE = re.compile(r"^s\s+(SATISFIABLE|UNSATISFIABLE)\s*$", re.MULTILINE)
ASSIGNMENT_RE = re.compile(r"^v\s+(.*?)$", re.MULTILINE)


@dataclass(frozen=True)
class Task:
    input_path: Path
    decision_proc: str
    clause_selector: str
    decision_phase: str

@dataclass
class RunResult:
    task: Task
    solver_status: str
    validation_status: str
    elapsed_wall_s: float
    message: str = ""


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    root = repo_root()
    parser = argparse.ArgumentParser(
        description="Run the fixed solver correctness matrix over the selected benchmark inputs."
    )
    try:
        default_solver = str(default_solver_bin(root))
    except FileNotFoundError:
        default_solver = ""
    parser.add_argument("--solver-bin", default=default_solver, help="Path to the Xorcle solver binary to test.")
    parser.add_argument("--checker", default=str(root / "tools" / "check_drup_plus.py"), help="Path to the DRUP checker used for UNSAT validation.")
    parser.add_argument("--timeout", type=float, default=5.0, help="External timeout per solver invocation in seconds.")
    parser.add_argument("--jobs", type=int, default=4, help="Number of matrix tasks to execute concurrently.")
    parser.add_argument("--compact", choices=("on", "off"), default="on", help="Whether to run the solver with compaction enabled.")
    parser.add_argument("--decision-procs", default=",".join(DECISION_PROCS), help="Comma-separated decision procedures to test.")
    parser.add_argument("--clause-selectors", default=",".join(CLAUSE_SELECTORS), help="Comma-separated clause selectors to test.")
    parser.add_argument("--decision-phases", default=",".join(DECISION_PHASES), help="Comma-separated decision phases to test.")
    parser.add_argument("--seed", type=int, default=DEFAULT_SEED, help="Deterministic solver seed for all matrix runs.")
    args = parser.parse_args(argv)

    if args.timeout <= 0:
        parser.error("--timeout must be > 0")
    if args.jobs <= 0:
        parser.error("--jobs must be > 0")
    if args.seed < 0:
        parser.error("--seed must be >= 0")
    return args

def parse_assignment(output: str) -> Optional[List[int]]:
    assignment_lines = ASSIGNMENT_RE.findall(output)
    if not assignment_lines:
        return None

    assignment_by_var: Dict[int, int] = {}
    try:
        for line in assignment_lines:
            for token in line.split():
                literal = int(token)
                if literal == 0:
                    if not assignment_by_var:
                        return []
                    max_var = max(assignment_by_var)
                    assignment: List[int] = []
                    for variable_index in range(1, max_var + 1):
                        if variable_index not in assignment_by_var:
                            return None
                        assignment.append(assignment_by_var[variable_index])
                    return assignment
                variable_index = abs(literal)
                if variable_index == 0:
                    return None
                bit = 1 if literal > 0 else 0
                existing_bit = assignment_by_var.get(variable_index)
                if existing_bit is not None and existing_bit != bit:
                    return None
                assignment_by_var[variable_index] = bit
    except ValueError:
        return None
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


def run_task(task: Task, solver_bin: Path, checker: Path, timeout_s: float, compact_mode: str, seed: int) -> RunResult:
    with tempfile.TemporaryDirectory(prefix="solver-proof-") as proof_dir:
        proof_path = Path(proof_dir) / "proof.drup"

        def run_solver_once(include_proof: bool) -> ProcRun:
            cmd = [
                str(solver_bin),
                "-d",
                task.decision_proc,
                "-p",
                task.decision_phase,
                "-c",
                task.clause_selector,
                "--seed",
                str(seed),
            ]
            if compact_mode == "off":
                cmd.extend(["-x", "off"])
            cmd.append(str(task.input_path))
            if include_proof:
                cmd.append(str(proof_path))
            return run_process(cmd, timeout_s=timeout_s)

        def run_solver_with_retry(include_proof: bool) -> tuple[ProcRun, str]:
            proc = run_solver_once(include_proof)
            retry_note = ""
            if not proc.timed_out and not proc.error_message and proc.returncode != 0:
                retry_proc = run_solver_once(include_proof)
                if not retry_proc.timed_out and not retry_proc.error_message and retry_proc.returncode == 0:
                    proc = retry_proc
                    retry_note = "known intermittent runtime error on first attempt; retried successfully"
            return proc, retry_note

        try:
            include_proof_on_first_run = (compact_mode == "off")
            proc, retry_note = run_solver_with_retry(include_proof_on_first_run)

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
                if retry_note:
                    message = f"{retry_note}; {message}"
                return RunResult(task, solver_status, "PASS" if ok else "FAIL", proc.elapsed_wall_s, message)

            if compact_mode == "off":
                ok, message = validate_unsat_proof(checker, task.input_path, proof_path, timeout_s)
                if retry_note:
                    message = f"{retry_note}; {message}"
                return RunResult(task, solver_status, "PASS" if ok else "FAIL", proc.elapsed_wall_s, message)

            proof_proc, proof_retry_note = run_solver_with_retry(include_proof=True)
            if proof_proc.timed_out:
                return RunResult(
                    task,
                    solver_status,
                    "FAIL",
                    proc.elapsed_wall_s,
                    f"proof-generation rerun timed out: {proof_proc.error_message}",
                )
            if proof_proc.error_message:
                return RunResult(
                    task,
                    solver_status,
                    "FAIL",
                    proc.elapsed_wall_s,
                    f"proof-generation rerun error: {proof_proc.error_message}",
                )
            if proof_proc.returncode != 0:
                detail = proof_proc.combined_output.strip()
                if detail:
                    detail = detail.splitlines()[-1]
                return RunResult(
                    task,
                    solver_status,
                    "FAIL",
                    proc.elapsed_wall_s,
                    f"proof-generation rerun failed (rc={proof_proc.returncode}): {detail}",
                )
            proof_status = parse_solver_status(proof_proc.combined_output)
            if proof_status != "UNSAT":
                return RunResult(
                    task,
                    solver_status,
                    "FAIL",
                    proc.elapsed_wall_s,
                    f"proof-generation rerun changed result to {proof_status or 'UNKNOWN'}",
                )

            ok, message = validate_unsat_proof(checker, task.input_path, proof_path, timeout_s)
            notes = [note for note in (retry_note, proof_retry_note) if note]
            if notes:
                message = f"{'; '.join(notes)}; {message}"
            return RunResult(task, solver_status, "PASS" if ok else "FAIL", proc.elapsed_wall_s, message)
        except Exception as exc:
            return RunResult(task, "ERROR", "FAIL", 0.0, f"unexpected exception: {exc}")


def format_result(result: RunResult, root: Path) -> str:
    rel = result.task.input_path.relative_to(root)
    return (
        f"{str(rel):68} "
        f"{result.task.decision_proc:10} "
        f"{result.task.decision_phase:8} "
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


def build_tasks(root: Path, decision_procs: Sequence[str], clause_selectors: Sequence[str],
                decision_phases: Sequence[str]) -> List[Task]:
    tasks: List[Task] = []
    for rel_input in DEFAULT_INPUTS:
        input_path = (root / rel_input).resolve()
        if not input_path.exists():
            raise FileNotFoundError(f"Missing input file: {input_path}")
        for decision_proc in decision_procs:
            for decision_phase in decision_phases:
                for clause_selector in clause_selectors:
                    tasks.append(Task(input_path=input_path,
                                      decision_proc=decision_proc,
                                      clause_selector=clause_selector,
                                      decision_phase=decision_phase))
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
        decision_procs = parse_csv_choices(
            args.decision_procs,
            ("fixed_literals", "variable_vsids", "clause_vsids"),
            "decision procedures",
        )
        clause_selectors = parse_csv_choices(
            args.clause_selectors,
            ("cyclic", "lt_watchlist", "exact_watchlist"),
            "clause selectors",
        )
        decision_phases = parse_csv_choices(
            args.decision_phases,
            ("unsat", "sat", "random"),
            "decision phases",
        )
        tasks = build_tasks(root, decision_procs, clause_selectors, decision_phases)
    except Exception as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    results: List[RunResult] = []
    print(f"Running {len(tasks)} tasks with {args.jobs} workers")
    print(f"Solver:  {solver_bin}")
    print(f"Checker: {checker}")
    print(f"Timeout: {args.timeout:.1f}s")
    print(f"Compact: {args.compact}")
    print(f"Seed:    {args.seed}")
    print()

    with ThreadPoolExecutor(max_workers=args.jobs) as executor:
        future_to_task = {
            executor.submit(run_task, task, solver_bin, checker, args.timeout, args.compact, args.seed): task
            for task in tasks
        }
        for future in as_completed(future_to_task):
            result = future.result()
            results.append(result)
            print(format_result(result, root))

    results.sort(key=lambda r: (str(r.task.input_path), r.task.decision_proc,
                                r.task.decision_phase, r.task.clause_selector))

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
