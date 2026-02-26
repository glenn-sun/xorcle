#!/usr/bin/env python3
from __future__ import annotations

import argparse
import csv
import itertools
import re
import statistics
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional, Sequence

sys.path.append(str(Path(__file__).resolve().parent))
from xnf_lib import Clause as XnfClause
from xnf_lib import Equation as XnfEquation
from xnf_lib import parse_xnf_file
from xnf_lib import write_cnf


SAT_RE = re.compile(r"^s\s+(SATISFIABLE|UNSATISFIABLE)\s*$", re.MULTILINE)
SOLVER_RESULT_RE = re.compile(r"^Result:\s*(SAT|UNSAT|TIMEOUT)\s*$", re.MULTILINE)
SOLVER_TIME_RE = re.compile(r"^Time:\s*([0-9]+(?:\.[0-9]+)?)\s+s\s*$", re.MULTILINE)

RAW_CSV_FIELDS = [
    "file",
    "input_format",
    "solver",
    "solver_config",
    "backend_input_kind",
    "run_index",
    "status",
    "elapsed_wall_s",
    "parsed_solver_time_s",
    "timeout_s",
    "returncode",
    "used_lift",
    "lift_k",
    "used_conversion",
    "conversion_format",
    "error_message",
]


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


def repo_root() -> Path:
    return Path(__file__).resolve().parents[1]


def split_main_and_solver_args(argv: Sequence[str]) -> tuple[list[str], list[str]]:
    if "--" not in argv:
        return list(argv), []
    idx = argv.index("--")
    return list(argv[:idx]), list(argv[idx + 1 :])


def parse_args(argv: Sequence[str]) -> tuple[argparse.Namespace, list[str]]:
    main_argv, solver_passthrough = split_main_and_solver_args(argv)
    root = repo_root()
    parser = argparse.ArgumentParser(
        description="Benchmark parity-sat-cpp vs Kissat and CryptoMiniSAT on CNF or XNF inputs."
    )
    parser.add_argument("input_path", help="Input file or directory")
    parser.add_argument("-i", "--input-format", required=True, metavar="{cnf,xnf}")
    parser.add_argument("-r", "--repeat", type=int, default=1, help="Script-level repeats per solver per file")
    parser.add_argument(
        "-t",
        "--timeout",
        type=float,
        default=10.0,
        help="Timeout in seconds for kissat and CryptoMiniSAT (default: 10.0)",
    )
    parser.add_argument("-b", "--solver-bin", default=str(root / "bin" / "main"))
    parser.add_argument("-k", "--kissat-bin", default=str(root / "external" / "kissat"))
    parser.add_argument("-m", "--cms-bin", default=str(root / "external" / "cryptominisat5"))
    parser.add_argument("-c", "--csv", dest="csv_path", default="", help="Optional CSV path for raw benchmark rows")
    parser.add_argument("-l", "--lift", type=int, default=None, help="Lift CNF to XNF using XORs of K fresh vars")
    parser.add_argument(
        "-w",
        "--write-lifted-xnf",
        action="store_true",
        help="When using --lift, write lifted XNF beside input as *_xnf.cnf",
    )
    parser.add_argument(
        "-e",
        "--exp-cnf",
        action="store_true",
        help="For XNF inputs, also benchmark naive no-new-vars exponential CNF encoding (including my solver)",
    )
    parser.add_argument(
        "-g",
        "--keep-going",
        action="store_true",
        default=True,
        help="Continue after per-file/per-solver errors (default behavior)",
    )
    args = parser.parse_args(main_argv)

    raw_input_format = str(args.input_format).strip().lower()
    if raw_input_format == "dimacs":
        args.input_format = "cnf"
    elif raw_input_format in {"cnf", "xnf"}:
        args.input_format = raw_input_format
    else:
        parser.error("--input-format must be one of: cnf, xnf")

    if args.repeat < 1:
        parser.error("--repeat must be >= 1")
    if args.timeout <= 0:
        parser.error("--timeout must be > 0")
    if args.lift is not None:
        if args.lift < 1:
            parser.error("--lift must be >= 1")
        if args.input_format != "cnf":
            parser.error("--lift is only valid with --input-format cnf")
    elif args.write_lifted_xnf:
        parser.error("--write-lifted-xnf requires --lift")
    if args.exp_cnf and not (args.input_format == "xnf" or args.lift is not None):
        parser.error("--exp-cnf requires XNF input (or --lift on CNF input)")

    validate_solver_passthrough(solver_passthrough, parser)
    return args, solver_passthrough


def validate_solver_passthrough(tokens: Sequence[str], parser: argparse.ArgumentParser) -> None:
    reserved_exact = {"--stats-file", "-s", "--runs", "-r", "--help", "-h"}
    reserved_prefix = ("--stats-file=", "--runs=")
    for tok in tokens:
        if tok in reserved_exact or any(tok.startswith(pfx) for pfx in reserved_prefix):
            parser.error(
                f"Solver pass-through contains reserved flag '{tok}'. "
                "The benchmark script manages --stats-file and --runs, and blocks --help."
            )
    # Intentionally not attempting to validate arbitrary positional solver args here.
    # The solver's option surface may evolve and the script supports arbitrary pass-through flags.


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
        elapsed = time.perf_counter() - start
        return ProcRun(
            returncode=cp.returncode,
            stdout=cp.stdout or "",
            stderr=cp.stderr or "",
            elapsed_wall_s=elapsed,
            timed_out=False,
        )
    except subprocess.TimeoutExpired as exc:
        elapsed = time.perf_counter() - start
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
            elapsed_wall_s=elapsed,
            timed_out=True,
            error_message=f"timeout after {timeout_s}s",
        )
    except OSError as exc:
        elapsed = time.perf_counter() - start
        return ProcRun(
            returncode=None,
            stdout="",
            stderr="",
            elapsed_wall_s=elapsed,
            timed_out=False,
            error_message=str(exc),
        )


def collect_input_files(input_path: Path) -> List[Path]:
    if not input_path.exists():
        raise FileNotFoundError(f"Input path does not exist: {input_path}")
    if input_path.is_file():
        return [input_path.resolve()]
    if input_path.is_dir():
        files = sorted(p.resolve() for p in input_path.rglob("*.cnf") if p.is_file())
        if not files:
            raise FileNotFoundError(f"No .cnf files found under directory: {input_path}")
        return files
    raise ValueError(f"Input path is neither file nor directory: {input_path}")


def parse_external_status(output: str) -> Optional[str]:
    m = SAT_RE.search(output)
    if not m:
        return None
    return "SAT" if m.group(1) == "SATISFIABLE" else "UNSAT"


def parse_solver_status_and_time_from_console(output: str) -> tuple[Optional[str], Optional[float]]:
    status = None
    t_s = None
    m_status = SOLVER_RESULT_RE.search(output)
    if m_status:
        status = m_status.group(1)
    m_time = SOLVER_TIME_RE.search(output)
    if m_time:
        try:
            t_s = float(m_time.group(1))
        except ValueError:
            t_s = None
    return status, t_s


def parse_my_solver_csv(csv_path: Path) -> tuple[Optional[str], Optional[float], str]:
    if not csv_path.exists():
        return None, None, f"missing solver stats CSV: {csv_path}"
    try:
        with csv_path.open("r", encoding="utf-8", newline="") as f:
            reader = csv.DictReader(f)
            rows = list(reader)
    except Exception as exc:
        return None, None, f"failed to read solver stats CSV: {exc}"
    if not rows:
        return None, None, "solver stats CSV was empty"
    row = rows[-1]
    status = row.get("result", "").strip().upper() or None
    t_s = None
    raw_time = (row.get("time_s") or "").strip()
    if raw_time:
        try:
            t_s = float(raw_time)
        except ValueError:
            pass
    return status, t_s, ""


def validate_standard_cnf_via_xnf_parser(input_file: Path) -> tuple[int, List[XnfClause]]:
    try:
        nvars, clauses = parse_xnf_file(str(input_file))
    except Exception as exc:
        raise ValueError(f"failed to parse CNF/XNF file for lifting: {exc}") from exc

    for cidx, clause in enumerate(clauses, start=1):
        for eidx, eq in enumerate(clause.equations, start=1):
            if len(eq.vars) != 1:
                raise ValueError(
                    f"--lift requires standard CNF; found linear/XNF equation with {len(eq.vars)} variables "
                    f"in clause {cidx}, token {eidx}"
                )
    return nvars, clauses


def lifted_var_block(v: int, k: int) -> List[int]:
    start = (v - 1) * k + 1
    return list(range(start, start + k))


def lifted_token_from_unary_equation(var: int, rhs: int, k: int) -> str:
    block = lifted_var_block(var, k)
    body = "+".join(str(x) for x in block)
    if rhs == 1:
        return body
    return "-" + body


def write_lifted_xnf(path: Path, orig_nvars: int, clauses: Sequence[XnfClause], k: int) -> None:
    lifted_nvars = orig_nvars * k
    with path.open("w", encoding="utf-8") as f:
        f.write(f"p cnf {lifted_nvars} {len(clauses)}\n")
        for clause in clauses:
            toks: List[str] = []
            for eq in clause.equations:
                if len(eq.vars) != 1:
                    raise ValueError("--lift writer received non-unary equation")
                toks.append(lifted_token_from_unary_equation(eq.vars[0], eq.rhs, k))
            f.write((" ".join(toks) + " 0\n") if toks else "0\n")


def lifted_output_filename(input_file: Path) -> Path:
    if input_file.suffix:
        return input_file.with_name(f"{input_file.stem}_xnf{input_file.suffix}")
    return input_file.with_name(f"{input_file.name}_xnf")


def evaluate_equation(eq: XnfEquation, assignment_map: Dict[int, int]) -> bool:
    parity = 0
    for v in eq.vars:
        parity ^= assignment_map.get(v, 0)
    return parity == eq.rhs


def clause_support_vars(clause: XnfClause) -> List[int]:
    support = set()
    for eq in clause.equations:
        support.update(eq.vars)
    return sorted(support)


def encode_xnf_clause_to_cnf_naive(clause: XnfClause) -> List[List[int]]:
    support = clause_support_vars(clause)
    out: List[List[int]] = []
    for bits in itertools.product((0, 1), repeat=len(support)):
        assignment = {v: bit for v, bit in zip(support, bits)}
        if any(evaluate_equation(eq, assignment) for eq in clause.equations):
            continue
        out.append([v if assignment[v] == 0 else -v for v in support])
    return out


def convert_xnf_to_cnf_naive(nvars: int, clauses: Sequence[XnfClause]) -> List[List[int]]:
    del nvars  # Header var count is preserved by caller; conversion uses no fresh variables.
    cnf_clauses: List[List[int]] = []
    for clause in clauses:
        cnf_clauses.extend(encode_xnf_clause_to_cnf_naive(clause))
    return cnf_clauses


def convert_xnf(converter_script: Path, fmt: str, src: Path, dst: Path) -> Optional[str]:
    cmd = [sys.executable, str(converter_script), fmt, str(src), str(dst)]
    proc = run_process(cmd, timeout_s=None)
    if proc.timed_out:
        return f"converter timed out ({fmt})"
    if proc.error_message:
        return f"converter error ({fmt}): {proc.error_message}"
    if proc.returncode != 0:
        tail = proc.combined_output[-400:].strip()
        return f"converter failed ({fmt}), rc={proc.returncode}: {tail}"
    if not dst.exists():
        return f"converter did not produce output file ({fmt}): {dst}"
    return None


def run_kissat_or_cms(
    solver_name: str,
    solver_config: str,
    backend_input_kind: str,
    bin_path: Path,
    input_file: Path,
    timeout_s: float,
    file_label: str,
    input_format: str,
    run_index: int,
    used_lift: bool,
    lift_k: Optional[int],
    used_conversion: bool,
    conversion_format: str,
) -> Dict[str, str]:
    cmd = [str(bin_path)]
    if solver_name == "kissat":
        cmd.append("--plain")
    cmd.append(str(input_file))
    proc = run_process(cmd, timeout_s=timeout_s)
    row = empty_raw_row(file_label, input_format, solver_name, solver_config, backend_input_kind, run_index)
    row["elapsed_wall_s"] = f"{proc.elapsed_wall_s:.6f}"
    row["timeout_s"] = f"{timeout_s:.6f}"
    row["returncode"] = "" if proc.returncode is None else str(proc.returncode)
    row["used_lift"] = "true" if used_lift else "false"
    row["lift_k"] = "" if lift_k is None else str(lift_k)
    row["used_conversion"] = "true" if used_conversion else "false"
    row["conversion_format"] = conversion_format

    if proc.timed_out:
        row["status"] = "TIMEOUT"
        row["error_message"] = proc.error_message
        return row
    if proc.error_message:
        row["status"] = "ERROR"
        row["error_message"] = proc.error_message
        return row

    status = parse_external_status(proc.combined_output)
    if status is None:
        row["status"] = "ERROR"
        row["error_message"] = f"could not parse SAT status from output (rc={proc.returncode})"
        return row
    row["status"] = status
    return row


def run_my_solver(
    solver_bin: Path,
    input_file: Path,
    solver_passthrough: Sequence[str],
    stats_csv_path: Path,
    file_label: str,
    input_format: str,
    run_index: int,
    used_lift: bool,
    lift_k: Optional[int],
    backend_input_kind: str,
    used_conversion: bool = False,
    conversion_format: str = "",
) -> Dict[str, str]:
    cmd = [
        str(solver_bin),
        *solver_passthrough,
        "--stats-file",
        str(stats_csv_path),
        "--runs",
        "1",
        str(input_file),
    ]
    proc = run_process(cmd, timeout_s=None)
    row = empty_raw_row(
        file_label,
        input_format,
        "my_solver",
        my_solver_config_name(backend_input_kind),
        backend_input_kind,
        run_index,
    )
    row["elapsed_wall_s"] = f"{proc.elapsed_wall_s:.6f}"
    row["timeout_s"] = ""
    row["returncode"] = "" if proc.returncode is None else str(proc.returncode)
    row["used_lift"] = "true" if used_lift else "false"
    row["lift_k"] = "" if lift_k is None else str(lift_k)
    row["used_conversion"] = "true" if used_conversion else "false"
    row["conversion_format"] = conversion_format

    if proc.error_message:
        row["status"] = "ERROR"
        row["error_message"] = proc.error_message
        return row

    status, parsed_t_s, csv_err = parse_my_solver_csv(stats_csv_path)
    if status is None:
        fallback_status, fallback_t_s = parse_solver_status_and_time_from_console(proc.combined_output)
        if fallback_status is None:
            row["status"] = "ERROR"
            err = csv_err or "could not parse solver status from CSV or console output"
            row["error_message"] = f"{err} (rc={proc.returncode})"
            return row
        status = fallback_status
        parsed_t_s = fallback_t_s

    row["status"] = normalize_status(status)
    if parsed_t_s is not None:
        row["parsed_solver_time_s"] = f"{parsed_t_s:.6f}"
    return row


def normalize_status(status: str) -> str:
    s = status.strip().upper()
    if s in {"SAT", "UNSAT", "TIMEOUT", "ERROR"}:
        return s
    return "ERROR"


def empty_raw_row(
    file_label: str,
    input_format: str,
    solver: str,
    solver_config: str,
    backend_input_kind: str,
    run_index: int,
) -> Dict[str, str]:
    return {
        "file": file_label,
        "input_format": input_format,
        "solver": solver,
        "solver_config": solver_config,
        "backend_input_kind": backend_input_kind,
        "run_index": str(run_index),
        "status": "",
        "elapsed_wall_s": "",
        "parsed_solver_time_s": "",
        "timeout_s": "",
        "returncode": "",
        "used_lift": "false",
        "lift_k": "",
        "used_conversion": "false",
        "conversion_format": "",
        "error_message": "",
    }


def summarize_rows(rows: List[Dict[str, str]]) -> Dict[str, Dict[str, object]]:
    grouped: Dict[str, Dict[str, List[Dict[str, str]]]] = {}
    for row in rows:
        solver_key = row.get("solver_config") or row["solver"]
        grouped.setdefault(row["file"], {}).setdefault(solver_key, []).append(row)

    summary: Dict[str, Dict[str, object]] = {}
    for file_label, solver_groups in grouped.items():
        file_summary: Dict[str, object] = {}
        for solver_name, srows in solver_groups.items():
            status_counts: Dict[str, int] = {}
            success_statuses = set()
            success_times: List[float] = []
            for row in srows:
                status = row["status"]
                status_counts[status] = status_counts.get(status, 0) + 1
                if status in {"SAT", "UNSAT"}:
                    success_statuses.add(status)
                    try:
                        success_times.append(float(row["elapsed_wall_s"]))
                    except ValueError:
                        pass

            inconsistent = len(success_statuses) > 1
            if inconsistent:
                summary_status = "INCONSISTENT"
            elif "SAT" in success_statuses:
                summary_status = "SAT"
            elif "UNSAT" in success_statuses:
                summary_status = "UNSAT"
            elif status_counts.get("TIMEOUT", 0) == len(srows):
                summary_status = "TIMEOUT"
            elif status_counts.get("ERROR", 0) == len(srows):
                summary_status = "ERROR"
            elif status_counts.get("TIMEOUT", 0) > 0:
                summary_status = "TIMEOUT"
            elif status_counts.get("ERROR", 0) > 0:
                summary_status = "ERROR"
            else:
                summary_status = "ERROR"

            median_s = statistics.median(success_times) if success_times else None
            best_s = min(success_times) if success_times else None

            file_summary[solver_name] = {
                "runs": len(srows),
                "completed_runs": sum(1 for row in srows if row["status"] in {"SAT", "UNSAT"}),
                "status_counts": status_counts,
                "status": summary_status,
                "median_time_s": median_s,
                "min_time_s": best_s,
                "inconsistent": inconsistent,
            }
        summary[file_label] = file_summary
    return summary


def print_summary(summary: Dict[str, Dict[str, object]], total_rows: int) -> None:
    files = sorted(summary.keys())
    any_inconsistent = False
    any_errors = False
    preferred_order = {
        "my_solver_cnf": 0,
        "my_solver_xnf": 0,
        "my_solver_cnf_exp": 1,
        "kissat_cnf": 2,
        "cryptominisat_cnf": 3,
        "cryptominisat_cnf_xor": 3,
        "kissat_cnf_exp": 4,
        "cryptominisat_cnf_exp": 5,
    }
    for file_label in files:
        print(f"\nFile: {file_label}")
        print(f"{'solver_config':24} {'status':13} {'median_s':>10} {'best_s':>10} {'timeouts':>8} {'errors':>6} {'runs':>6}")
        print("-" * 88)
        per_solver = summary[file_label]
        for solver_name in sorted(per_solver.keys(), key=lambda s: (preferred_order.get(s, 99), s)):
            item = per_solver[solver_name]
            counts = item["status_counts"]  # type: ignore[index]
            status = str(item["status"])
            median_s = item["median_time_s"]  # type: ignore[index]
            best_s = item["min_time_s"]  # type: ignore[index]
            timeouts = int(counts.get("TIMEOUT", 0))  # type: ignore[union-attr]
            errors = int(counts.get("ERROR", 0))  # type: ignore[union-attr]
            runs = int(item["runs"])  # type: ignore[arg-type]
            any_inconsistent = any_inconsistent or bool(item["inconsistent"])  # type: ignore[arg-type]
            any_errors = any_errors or errors > 0
            print(
                f"{solver_name:24} {status:13} "
                f"{format_float(median_s):>10} {format_float(best_s):>10} "
                f"{timeouts:8d} {errors:6d} {runs:6d}"
            )

    print("\nSummary:")
    print(f"  files benchmarked: {len(files)}")
    print(f"  total raw runs: {total_rows}")
    print(f"  inconsistencies detected: {'yes' if any_inconsistent else 'no'}")
    print(f"  errors detected: {'yes' if any_errors else 'no'}")


def format_float(value: object) -> str:
    if value is None:
        return "-"
    return f"{float(value):.4f}"


def write_raw_csv(path: Path, rows: List[Dict[str, str]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=RAW_CSV_FIELDS)
        writer.writeheader()
        writer.writerows(rows)


def my_solver_config_name(backend_input_kind: str) -> str:
    if backend_input_kind == "cnf-exp":
        return "my_solver_cnf_exp"
    return f"my_solver_{backend_input_kind}"


def external_solver_config_name(solver_name: str, backend_input_kind: str) -> str:
    if solver_name == "kissat":
        return "kissat_cnf_exp" if backend_input_kind == "cnf-exp" else "kissat_cnf"
    if solver_name == "cryptominisat":
        if backend_input_kind == "cnf-xor":
            return "cryptominisat_cnf_xor"
        if backend_input_kind == "cnf-exp":
            return "cryptominisat_cnf_exp"
        return "cryptominisat_cnf"
    return f"{solver_name}_{backend_input_kind}"


def make_external_error_row(
    file_label: str,
    input_format: str,
    solver_name: str,
    backend_input_kind: str,
    run_idx: int,
    timeout_s: float,
    used_lift: bool,
    lift_k: Optional[int],
    used_conversion: bool,
    conversion_format: str,
    error_message: str,
) -> Dict[str, str]:
    row = empty_raw_row(
        file_label,
        input_format,
        solver_name,
        external_solver_config_name(solver_name, backend_input_kind),
        backend_input_kind,
        run_idx,
    )
    row["status"] = "ERROR"
    row["elapsed_wall_s"] = "0.000000"
    row["timeout_s"] = f"{timeout_s:.6f}"
    row["used_lift"] = "true" if used_lift else "false"
    row["lift_k"] = "" if lift_k is None else str(lift_k)
    row["used_conversion"] = "true" if used_conversion else "false"
    row["conversion_format"] = conversion_format
    row["error_message"] = error_message
    return row


def benchmark_file(
    file_path: Path,
    args: argparse.Namespace,
    solver_passthrough: Sequence[str],
    converter_script: Path,
) -> List[Dict[str, str]]:
    file_rows: List[Dict[str, str]] = []
    file_label = str(file_path)
    with tempfile.TemporaryDirectory(prefix="bench_solvers_") as tmpdir:
        tmpdir_path = Path(tmpdir)
        effective_input_file = file_path
        effective_input_format = args.input_format
        used_lift = False
        lift_k: Optional[int] = None

        if args.lift is not None:
            orig_nvars, lifted_clauses = validate_standard_cnf_via_xnf_parser(file_path)
            lifted_file = tmpdir_path / "lifted_input.xnf"
            write_lifted_xnf(lifted_file, orig_nvars, lifted_clauses, args.lift)
            if args.write_lifted_xnf:
                persisted_lifted = lifted_output_filename(file_path)
                write_lifted_xnf(persisted_lifted, orig_nvars, lifted_clauses, args.lift)
                print(f"Wrote lifted XNF: {persisted_lifted}")
            effective_input_file = lifted_file
            effective_input_format = "xnf"
            used_lift = True
            lift_k = args.lift

        kissat_input = effective_input_file
        cms_input = effective_input_file
        kissat_conversion_err = ""
        cms_conversion_err = ""
        kissat_conversion_used = False
        cms_conversion_used = False
        kissat_conv_fmt = ""
        cms_conv_fmt = ""
        exp_cnf_enabled = args.exp_cnf and effective_input_format == "xnf"
        exp_cnf_input = tmpdir_path / "exp_input.cnf"
        exp_cnf_err = ""

        if effective_input_format == "xnf":
            kissat_input = tmpdir_path / "kissat_input.cnf"
            cms_input = tmpdir_path / "cms_input.cnf"
            kissat_conversion_err = convert_xnf(converter_script, "cnf", effective_input_file, kissat_input) or ""
            cms_conversion_err = convert_xnf(converter_script, "cnf-xor", effective_input_file, cms_input) or ""
            kissat_conversion_used = True
            cms_conversion_used = True
            kissat_conv_fmt = "cnf"
            cms_conv_fmt = "cnf-xor"
            if exp_cnf_enabled:
                try:
                    exp_nvars, exp_xnf_clauses = parse_xnf_file(str(effective_input_file))
                    exp_cnf_clauses = convert_xnf_to_cnf_naive(exp_nvars, exp_xnf_clauses)
                    write_cnf(str(exp_cnf_input), exp_nvars, exp_cnf_clauses)
                except Exception as exc:
                    exp_cnf_err = f"naive exponential CNF generation failed: {exc}"

        for run_idx in range(1, args.repeat + 1):
            my_stats_csv = tmpdir_path / f"my_solver_stats_{run_idx}.csv"
            my_row = run_my_solver(
                solver_bin=Path(args.solver_bin),
                input_file=effective_input_file,
                solver_passthrough=solver_passthrough,
                stats_csv_path=my_stats_csv,
                file_label=file_label,
                input_format=args.input_format,
                run_index=run_idx,
                used_lift=used_lift,
                lift_k=lift_k,
                backend_input_kind=effective_input_format,
            )
            file_rows.append(my_row)

            if kissat_conversion_err:
                file_rows.append(
                    make_external_error_row(
                        file_label=file_label,
                        input_format=args.input_format,
                        solver_name="kissat",
                        backend_input_kind="cnf",
                        run_idx=run_idx,
                        timeout_s=args.timeout,
                        used_lift=used_lift,
                        lift_k=lift_k,
                        used_conversion=True,
                        conversion_format=kissat_conv_fmt,
                        error_message=kissat_conversion_err,
                    )
                )
            else:
                file_rows.append(
                    run_kissat_or_cms(
                        solver_name="kissat",
                        solver_config=external_solver_config_name("kissat", "cnf"),
                        backend_input_kind="cnf",
                        bin_path=Path(args.kissat_bin),
                        input_file=kissat_input,
                        timeout_s=args.timeout,
                        file_label=file_label,
                        input_format=args.input_format,
                        run_index=run_idx,
                        used_lift=used_lift,
                        lift_k=lift_k,
                        used_conversion=kissat_conversion_used,
                        conversion_format=kissat_conv_fmt,
                    )
                )

            cms_backend_kind = "cnf-xor" if effective_input_format == "xnf" else "cnf"
            if cms_conversion_err:
                file_rows.append(
                    make_external_error_row(
                        file_label=file_label,
                        input_format=args.input_format,
                        solver_name="cryptominisat",
                        backend_input_kind=cms_backend_kind,
                        run_idx=run_idx,
                        timeout_s=args.timeout,
                        used_lift=used_lift,
                        lift_k=lift_k,
                        used_conversion=bool(cms_conv_fmt),
                        conversion_format=cms_conv_fmt,
                        error_message=cms_conversion_err,
                    )
                )
            else:
                file_rows.append(
                    run_kissat_or_cms(
                        solver_name="cryptominisat",
                        solver_config=external_solver_config_name("cryptominisat", cms_backend_kind),
                        backend_input_kind=cms_backend_kind,
                        bin_path=Path(args.cms_bin),
                        input_file=cms_input,
                        timeout_s=args.timeout,
                        file_label=file_label,
                        input_format=args.input_format,
                        run_index=run_idx,
                        used_lift=used_lift,
                        lift_k=lift_k,
                        used_conversion=cms_conversion_used,
                        conversion_format=cms_conv_fmt,
                    )
                )

            if exp_cnf_enabled:
                if exp_cnf_err:
                    row = empty_raw_row(
                        file_label,
                        args.input_format,
                        "my_solver",
                        my_solver_config_name("cnf-exp"),
                        "cnf-exp",
                        run_idx,
                    )
                    row["status"] = "ERROR"
                    row["used_lift"] = "true" if used_lift else "false"
                    row["lift_k"] = "" if lift_k is None else str(lift_k)
                    row["used_conversion"] = "true"
                    row["conversion_format"] = "cnf-exp"
                    row["error_message"] = exp_cnf_err
                    file_rows.append(row)
                    file_rows.append(
                        make_external_error_row(
                            file_label=file_label,
                            input_format=args.input_format,
                            solver_name="kissat",
                            backend_input_kind="cnf-exp",
                            run_idx=run_idx,
                            timeout_s=args.timeout,
                            used_lift=used_lift,
                            lift_k=lift_k,
                            used_conversion=True,
                            conversion_format="cnf-exp",
                            error_message=exp_cnf_err,
                        )
                    )
                    file_rows.append(
                        make_external_error_row(
                            file_label=file_label,
                            input_format=args.input_format,
                            solver_name="cryptominisat",
                            backend_input_kind="cnf-exp",
                            run_idx=run_idx,
                            timeout_s=args.timeout,
                            used_lift=used_lift,
                            lift_k=lift_k,
                            used_conversion=True,
                            conversion_format="cnf-exp",
                            error_message=exp_cnf_err,
                        )
                    )
                else:
                    file_rows.append(
                        run_my_solver(
                            solver_bin=Path(args.solver_bin),
                            input_file=exp_cnf_input,
                            solver_passthrough=solver_passthrough,
                            stats_csv_path=tmpdir_path / f"my_solver_exp_stats_{run_idx}.csv",
                            file_label=file_label,
                            input_format=args.input_format,
                            run_index=run_idx,
                            used_lift=used_lift,
                            lift_k=lift_k,
                            backend_input_kind="cnf-exp",
                            used_conversion=True,
                            conversion_format="cnf-exp",
                        )
                    )
                    file_rows.append(
                        run_kissat_or_cms(
                            solver_name="kissat",
                            solver_config=external_solver_config_name("kissat", "cnf-exp"),
                            backend_input_kind="cnf-exp",
                            bin_path=Path(args.kissat_bin),
                            input_file=exp_cnf_input,
                            timeout_s=args.timeout,
                            file_label=file_label,
                            input_format=args.input_format,
                            run_index=run_idx,
                            used_lift=used_lift,
                            lift_k=lift_k,
                            used_conversion=True,
                            conversion_format="cnf-exp",
                        )
                    )
                    file_rows.append(
                        run_kissat_or_cms(
                            solver_name="cryptominisat",
                            solver_config=external_solver_config_name("cryptominisat", "cnf-exp"),
                            backend_input_kind="cnf-exp",
                            bin_path=Path(args.cms_bin),
                            input_file=exp_cnf_input,
                            timeout_s=args.timeout,
                            file_label=file_label,
                            input_format=args.input_format,
                            run_index=run_idx,
                            used_lift=used_lift,
                            lift_k=lift_k,
                            used_conversion=True,
                            conversion_format="cnf-exp",
                        )
                    )

    return file_rows


def default_solver_routes_for_args(args: argparse.Namespace) -> List[tuple[str, str, str]]:
    effective_is_xnf = args.input_format == "xnf" or args.lift is not None
    if effective_is_xnf:
        routes: List[tuple[str, str, str]] = [
            ("my_solver", "my_solver_xnf", "xnf"),
            ("kissat", "kissat_cnf", "cnf"),
            ("cryptominisat", "cryptominisat_cnf_xor", "cnf-xor"),
        ]
    else:
        routes = [
            ("my_solver", "my_solver_cnf", "cnf"),
            ("kissat", "kissat_cnf", "cnf"),
            ("cryptominisat", "cryptominisat_cnf", "cnf"),
        ]
    if args.exp_cnf and effective_is_xnf:
        routes.extend(
            [
                ("my_solver", "my_solver_cnf_exp", "cnf-exp"),
                ("kissat", "kissat_cnf_exp", "cnf-exp"),
                ("cryptominisat", "cryptominisat_cnf_exp", "cnf-exp"),
            ]
        )
    return routes


def main(argv: Sequence[str]) -> int:
    try:
        args, solver_passthrough = parse_args(argv)
        input_files = collect_input_files(Path(args.input_path))
    except (SystemExit, FileNotFoundError, ValueError) as exc:
        if isinstance(exc, SystemExit):
            raise
        print(f"Error: {exc}", file=sys.stderr)
        return 2

    converter_script = repo_root() / "tools" / "xnf_convert.py"
    if (args.input_format == "xnf" or args.lift is not None) and not converter_script.exists():
        print(f"Error: converter script not found: {converter_script}", file=sys.stderr)
        return 2

    raw_rows: List[Dict[str, str]] = []
    fatal_error = False
    for file_path in input_files:
        try:
            raw_rows.extend(benchmark_file(file_path, args, solver_passthrough, converter_script))
        except Exception as exc:
            if not args.keep_going:
                print(f"Fatal error while benchmarking {file_path}: {exc}", file=sys.stderr)
                return 2
            fatal_error = True
            # Emit ERROR rows for all solvers for this file/run if something unexpected happened.
            for run_idx in range(1, args.repeat + 1):
                for solver_name, solver_config, backend_input_kind in default_solver_routes_for_args(args):
                    row = empty_raw_row(
                        str(file_path),
                        args.input_format,
                        solver_name,
                        solver_config,
                        backend_input_kind,
                        run_idx,
                    )
                    row["status"] = "ERROR"
                    row["error_message"] = f"unexpected benchmark error: {exc}"
                    raw_rows.append(row)

    if not raw_rows:
        print("Error: no benchmark rows produced", file=sys.stderr)
        return 2

    summary = summarize_rows(raw_rows)
    print_summary(summary, total_rows=len(raw_rows))

    if args.csv_path:
        write_raw_csv(Path(args.csv_path), raw_rows)
        print(f"\nRaw CSV written to: {args.csv_path}")

    return 0 if not fatal_error else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
