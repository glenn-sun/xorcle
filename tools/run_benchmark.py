#!/usr/bin/env python3
from __future__ import annotations

import argparse
import csv
import re
import statistics
import subprocess
import sys
import tempfile
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional, Sequence

sys.path.append(str(Path(__file__).resolve().parent))
from benchmark_orchestration import DEFAULT_SOLVER_LINES
from xnf_lib import convert_xnf_to_cnf_naive
from xnf_lib import parse_xnf_file
from xnf_lib import write_cnf


SAT_RE = re.compile(r"^s\s+(SATISFIABLE|UNSATISFIABLE)\s*$", re.MULTILINE)
SOLVER_RESULT_RE = re.compile(r"^Result:\s*(SAT|UNSAT|TIMEOUT)\s*$", re.MULTILINE)
SOLVER_TIME_RE = re.compile(r"^Time:\s*([0-9]+(?:\.[0-9]+)?)\s+s\s*$", re.MULTILINE)
SOLVER_DECISIONS_RE = re.compile(r"(?im)^\s*(?:c\s+)?decisions?\s*:\s*([0-9]+)\b")

RAW_CSV_FIELDS = [
    "file",
    "sample_key",
    "input_format",
    "solver",
    "solver_config",
    "backend_input_kind",
    "run_index",
    "status",
    "elapsed_wall_s",
    "parsed_solver_time_s",
    "parsed_solver_decisions",
    "timeout_s",
    "returncode",
    "used_lift",
    "lift_k",
    "used_conversion",
    "conversion_format",
    "error_message",
]

SOLVER_CHOICES = (
    "xorcle_0_1",
    "2xornado",
    "xorricane",
    "kissat_plain",
    "kissat_default",
    "cryptominisat",
)
XORCLE_PUBLIC_NAME = "Xorcle 0.1"


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
        description="Benchmark parity-sat-cpp and external SAT solvers on CNF or XNF inputs."
    )
    parser.add_argument("input_path", help="Input file or directory")
    parser.add_argument("-i", "--input-format", required=True, metavar="{cnf,xnf}")
    parser.add_argument("-r", "--repeat", type=int, default=1, help="Solver reruns on the same file per solver")
    parser.add_argument(
        "-t",
        "--timeout",
        type=float,
        default=5.0,
        help="Timeout in seconds for all solver runs (default: 5.0)",
    )
    parser.add_argument("-b", "--solver-bin", default=str(root / "bin" / "main-release"))
    parser.add_argument("-k", "--kissat-bin", default=str(root / "external" / "kissat"))
    parser.add_argument("-m", "--cms-bin", default=str(root / "external" / "cryptominisat5"))
    parser.add_argument("--2xornado-bin", dest="xornado_bin", default=str(root / "external" / "2xornado"))
    parser.add_argument("--xorricane-bin", default=str(root / "external" / "xorricane"))
    parser.add_argument("-c", "--csv", dest="csv_path", default="", help="Optional CSV path for raw benchmark rows")
    parser.add_argument(
        "--kissat-mode",
        choices=["plain", "default", "both"],
        default="plain",
        help="Kissat run mode: --plain only, default mode only, or both (default: plain)",
    )
    parser.add_argument(
        "-e",
        "--exp-cnf",
        action="store_true",
        help=f"For XNF inputs, also benchmark naive no-new-vars exponential CNF encoding (including {XORCLE_PUBLIC_NAME})",
    )
    parser.add_argument(
        "-g",
        "--keep-going",
        action="store_true",
        default=True,
        help="Continue after per-file/per-solver errors (default behavior)",
    )
    parser.add_argument(
        "--jobs",
        type=int,
        default=4,
        help="Number of files to benchmark concurrently (default: 4)",
    )
    parser.add_argument(
        "--solvers",
        nargs="+",
        choices=SOLVER_CHOICES,
        default=list(DEFAULT_SOLVER_LINES),
        help="Subset of solver routes to execute",
    )
    parser.add_argument(
        "--saveconverted",
        action="store_true",
        help="Persist converted CNF/CNF-XOR/2-XNF files under tests/converted/<type>/.",
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
    if args.jobs < 1:
        parser.error("--jobs must be >= 1")
    if args.timeout <= 0:
        parser.error("--timeout must be > 0")
    if args.exp_cnf and args.input_format != "xnf":
        parser.error("--exp-cnf requires --input-format xnf")

    validate_solver_passthrough(solver_passthrough, parser)
    return args, solver_passthrough


def validate_solver_passthrough(tokens: Sequence[str], parser: argparse.ArgumentParser) -> None:
    reserved_exact = {"--stats-file", "-s", "--help", "-h"}
    reserved_prefix = ("--stats-file=",)
    for tok in tokens:
        if tok in reserved_exact or any(tok.startswith(pfx) for pfx in reserved_prefix):
            parser.error(
                f"Solver pass-through contains reserved flag '{tok}'. "
                "The benchmark script manages --stats-file and blocks --help."
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


def parse_decisions_from_output(output: str) -> Optional[int]:
    m = SOLVER_DECISIONS_RE.search(output)
    if not m:
        return None
    try:
        return int(m.group(1))
    except ValueError:
        return None


def parse_xorcle_csv(csv_path: Path) -> tuple[Optional[str], Optional[float], Optional[int], str]:
    if not csv_path.exists():
        return None, None, None, f"missing solver stats CSV: {csv_path}"
    try:
        with csv_path.open("r", encoding="utf-8", newline="") as f:
            reader = csv.DictReader(f)
            rows = list(reader)
    except Exception as exc:
        return None, None, None, f"failed to read solver stats CSV: {exc}"
    if not rows:
        return None, None, None, "solver stats CSV was empty"
    row = rows[-1]
    status = row.get("result", "").strip().upper() or None
    t_s = None
    decisions = None
    raw_time = (row.get("time_s") or "").strip()
    if raw_time:
        try:
            t_s = float(raw_time)
        except ValueError:
            pass
    raw_decisions = (row.get("decisions") or "").strip()
    if raw_decisions:
        try:
            decisions = int(raw_decisions)
        except ValueError:
            pass
    return status, t_s, decisions, ""


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


def persist_converted_file(*, converted_file: Path, source_input: Path, converted_kind: str) -> Optional[str]:
    root = repo_root()
    tests_root = (root / "tests").resolve()
    source_resolved = source_input.resolve()
    try:
        rel = source_resolved.relative_to(tests_root)
    except ValueError:
        rel = Path(source_input.name)
    out_path = root / "tests" / "converted" / converted_kind / rel
    out_path.parent.mkdir(parents=True, exist_ok=True)
    try:
        out_path.write_bytes(converted_file.read_bytes())
    except Exception as exc:
        return f"failed to persist converted file '{converted_file}' -> '{out_path}': {exc}"
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
    kissat_plain: Optional[bool] = None,
) -> Dict[str, str]:
    cmd = [str(bin_path)]
    if solver_name == "kissat" and kissat_plain:
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
    parsed_decisions = parse_decisions_from_output(proc.combined_output)
    if parsed_decisions is not None:
        row["parsed_solver_decisions"] = str(parsed_decisions)
    return row


def run_xorcle_solver(
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
    timeout_s: float,
    used_conversion: bool = False,
    conversion_format: str = "",
) -> Dict[str, str]:
    cmd = [
        str(solver_bin),
        *solver_passthrough,
        "--stats-file",
        str(stats_csv_path),
        str(input_file),
    ]
    proc = run_process(cmd, timeout_s=timeout_s)
    row = empty_raw_row(
        file_label,
        input_format,
        "xorcle_0_1",
        xorcle_config_name(backend_input_kind),
        backend_input_kind,
        run_index,
    )
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

    status, parsed_t_s, parsed_decisions, csv_err = parse_xorcle_csv(stats_csv_path)
    if status is None:
        fallback_status, fallback_t_s = parse_solver_status_and_time_from_console(proc.combined_output)
        if fallback_status is None:
            row["status"] = "ERROR"
            err = csv_err or "could not parse solver status from CSV or console output"
            row["error_message"] = f"{err} (rc={proc.returncode})"
            return row
        status = fallback_status
        parsed_t_s = fallback_t_s
    if parsed_decisions is None:
        parsed_decisions = parse_decisions_from_output(proc.combined_output)

    row["status"] = normalize_status(status)
    if parsed_t_s is not None:
        row["parsed_solver_time_s"] = f"{parsed_t_s:.6f}"
    if parsed_decisions is not None:
        row["parsed_solver_decisions"] = str(parsed_decisions)
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
        "sample_key": "",
        "input_format": input_format,
        "solver": solver,
        "solver_config": solver_config,
        "backend_input_kind": backend_input_kind,
        "run_index": str(run_index),
        "status": "",
        "elapsed_wall_s": "",
        "parsed_solver_time_s": "",
        "parsed_solver_decisions": "",
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
        "xorcle_0_1_cnf": 0,
        "xorcle_0_1_xnf": 0,
        "2xornado_2xnf": 1,
        "xorricane_xnf": 2,
        "xorricane_cnf": 2,
        "xorcle_0_1_cnf_exp": 3,
        "kissat_plain_cnf": 4,
        "kissat_default_cnf": 5,
        "kissat_cnf": 4,
        "cryptominisat_cnf": 6,
        "cryptominisat_cnf_xor": 6,
        "xorricane_cnf_exp": 7,
        "kissat_plain_cnf_exp": 8,
        "kissat_default_cnf_exp": 9,
        "kissat_cnf_exp": 8,
        "cryptominisat_cnf_exp": 10,
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


def xorcle_config_name(backend_input_kind: str) -> str:
    if backend_input_kind == "cnf-exp":
        return "xorcle_0_1_cnf_exp"
    return f"xorcle_0_1_{backend_input_kind}"


def external_solver_config_name(solver_name: str, backend_input_kind: str, kissat_plain: Optional[bool] = None) -> str:
    if solver_name == "kissat":
        suffix = "cnf_exp" if backend_input_kind == "cnf-exp" else "cnf"
        if kissat_plain is None:
            return f"kissat_{suffix}"
        mode = "plain" if kissat_plain else "default"
        return f"kissat_{mode}_{suffix}"
    if solver_name == "cryptominisat":
        if backend_input_kind == "cnf-xor":
            return "cryptominisat_cnf_xor"
        if backend_input_kind == "cnf-exp":
            return "cryptominisat_cnf_exp"
        return "cryptominisat_cnf"
    if solver_name == "2xornado":
        return "2xornado_2xnf"
    if solver_name == "xorricane":
        if backend_input_kind == "cnf-exp":
            return "xorricane_cnf_exp"
        if backend_input_kind == "xnf":
            return "xorricane_xnf"
        return "xorricane_cnf"
    suffix = backend_input_kind.replace("-", "_")
    return f"{solver_name}_{suffix}"


def selected_kissat_modes(args: argparse.Namespace) -> List[bool]:
    wants_plain = "kissat_plain" in args.solvers
    wants_default = "kissat_default" in args.solvers
    if args.kissat_mode == "plain":
        return [True] if wants_plain else []
    if args.kissat_mode == "default":
        return [False] if wants_default else []
    modes: List[bool] = []
    if wants_plain:
        modes.append(True)
    if wants_default:
        modes.append(False)
    return modes


def make_external_error_row(
    file_label: str,
    input_format: str,
    solver_name: str,
    solver_config: str,
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
        solver_config,
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

        kissat_input = effective_input_file
        cms_input = effective_input_file
        xornado_input = tmpdir_path / "2xornado_input.cnf"
        kissat_conversion_err = ""
        cms_conversion_err = ""
        xornado_conversion_err = ""
        kissat_conversion_used = False
        cms_conversion_used = False
        xornado_conversion_used = False
        kissat_conv_fmt = ""
        cms_conv_fmt = ""
        xornado_conv_fmt = "2-xnf"
        exp_cnf_enabled = args.exp_cnf and effective_input_format == "xnf"
        exp_cnf_input = tmpdir_path / "exp_input.cnf"
        exp_cnf_err = ""

        if "2xornado" in args.solvers:
            xornado_conversion_err = convert_xnf(converter_script, "2-xnf", effective_input_file, xornado_input) or ""
            xornado_conversion_used = True
            if args.saveconverted and not xornado_conversion_err:
                save_err = persist_converted_file(
                    converted_file=xornado_input,
                    source_input=effective_input_file,
                    converted_kind="2_xnf",
                )
                if save_err:
                    xornado_conversion_err = save_err

        if effective_input_format == "xnf":
            kissat_input = tmpdir_path / "kissat_input.cnf"
            cms_input = tmpdir_path / "cms_input.cnf"
            kissat_conversion_err = convert_xnf(converter_script, "cnf", effective_input_file, kissat_input) or ""
            cms_conversion_err = convert_xnf(converter_script, "cnf-xor", effective_input_file, cms_input) or ""
            kissat_conversion_used = True
            cms_conversion_used = True
            kissat_conv_fmt = "cnf"
            cms_conv_fmt = "cnf-xor"
            if args.saveconverted and not kissat_conversion_err:
                save_err = persist_converted_file(
                    converted_file=kissat_input,
                    source_input=effective_input_file,
                    converted_kind="cnf",
                )
                if save_err:
                    kissat_conversion_err = save_err
            if args.saveconverted and not cms_conversion_err:
                save_err = persist_converted_file(
                    converted_file=cms_input,
                    source_input=effective_input_file,
                    converted_kind="cnf_xor",
                )
                if save_err:
                    cms_conversion_err = save_err
            if exp_cnf_enabled:
                try:
                    exp_nvars, exp_xnf_clauses = parse_xnf_file(str(effective_input_file))
                    exp_cnf_clauses = convert_xnf_to_cnf_naive(exp_nvars, exp_xnf_clauses)
                    write_cnf(str(exp_cnf_input), exp_nvars, exp_cnf_clauses)
                    if args.saveconverted:
                        save_err = persist_converted_file(
                            converted_file=exp_cnf_input,
                            source_input=effective_input_file,
                            converted_kind="cnf_exp",
                        )
                        if save_err:
                            exp_cnf_err = save_err
                except Exception as exc:
                    exp_cnf_err = f"naive exponential CNF generation failed: {exc}"

        for run_idx in range(1, args.repeat + 1):
            if "xorcle_0_1" in args.solvers:
                my_stats_csv = tmpdir_path / f"xorcle_0_1_stats_{run_idx}.csv"
                my_row = run_xorcle_solver(
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
                    timeout_s=args.timeout,
                )
                file_rows.append(my_row)

            if "2xornado" in args.solvers:
                xornado_solver_config = external_solver_config_name("2xornado", "2xnf")
                if xornado_conversion_err:
                    file_rows.append(
                        make_external_error_row(
                            file_label=file_label,
                            input_format=args.input_format,
                            solver_name="2xornado",
                            solver_config=xornado_solver_config,
                            backend_input_kind="2xnf",
                            run_idx=run_idx,
                            timeout_s=args.timeout,
                            used_lift=used_lift,
                            lift_k=lift_k,
                            used_conversion=True,
                            conversion_format=xornado_conv_fmt,
                            error_message=xornado_conversion_err,
                        )
                    )
                else:
                    file_rows.append(
                        run_kissat_or_cms(
                            solver_name="2xornado",
                            solver_config=xornado_solver_config,
                            backend_input_kind="2xnf",
                            bin_path=Path(args.xornado_bin),
                            input_file=xornado_input,
                            timeout_s=args.timeout,
                            file_label=file_label,
                            input_format=args.input_format,
                            run_index=run_idx,
                            used_lift=used_lift,
                            lift_k=lift_k,
                            used_conversion=xornado_conversion_used,
                            conversion_format=xornado_conv_fmt,
                        )
                    )

            if "xorricane" in args.solvers:
                file_rows.append(
                    run_kissat_or_cms(
                        solver_name="xorricane",
                        solver_config=external_solver_config_name("xorricane", effective_input_format),
                        backend_input_kind=effective_input_format,
                        bin_path=Path(args.xorricane_bin),
                        input_file=effective_input_file,
                        timeout_s=args.timeout,
                        file_label=file_label,
                        input_format=args.input_format,
                        run_index=run_idx,
                        used_lift=used_lift,
                        lift_k=lift_k,
                        used_conversion=False,
                        conversion_format="",
                    )
                )

            for kissat_plain in selected_kissat_modes(args):
                kissat_solver_config = external_solver_config_name("kissat", "cnf", kissat_plain=kissat_plain)
                if kissat_conversion_err:
                    file_rows.append(
                        make_external_error_row(
                            file_label=file_label,
                            input_format=args.input_format,
                            solver_name="kissat",
                            solver_config=kissat_solver_config,
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
                            solver_config=kissat_solver_config,
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
                            kissat_plain=kissat_plain,
                        )
                    )

            if "cryptominisat" in args.solvers:
                cms_backend_kind = "cnf-xor" if effective_input_format == "xnf" else "cnf"
                if cms_conversion_err:
                    file_rows.append(
                        make_external_error_row(
                            file_label=file_label,
                            input_format=args.input_format,
                            solver_name="cryptominisat",
                            solver_config=external_solver_config_name("cryptominisat", cms_backend_kind),
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
                    if "xorcle_0_1" in args.solvers:
                        row = empty_raw_row(
                            file_label,
                            args.input_format,
                            "xorcle_0_1",
                            xorcle_config_name("cnf-exp"),
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
                    for kissat_plain in selected_kissat_modes(args):
                        file_rows.append(
                            make_external_error_row(
                                file_label=file_label,
                                input_format=args.input_format,
                                solver_name="kissat",
                                solver_config=external_solver_config_name("kissat", "cnf-exp", kissat_plain=kissat_plain),
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
                    if "cryptominisat" in args.solvers:
                        file_rows.append(
                            make_external_error_row(
                                file_label=file_label,
                                input_format=args.input_format,
                                solver_name="cryptominisat",
                                solver_config=external_solver_config_name("cryptominisat", "cnf-exp"),
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
                    if "xorricane" in args.solvers:
                        file_rows.append(
                            make_external_error_row(
                                file_label=file_label,
                                input_format=args.input_format,
                                solver_name="xorricane",
                                solver_config=external_solver_config_name("xorricane", "cnf-exp"),
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
                    if "xorcle_0_1" in args.solvers:
                        file_rows.append(
                            run_xorcle_solver(
                                solver_bin=Path(args.solver_bin),
                                input_file=exp_cnf_input,
                                solver_passthrough=solver_passthrough,
                                stats_csv_path=tmpdir_path / f"xorcle_0_1_exp_stats_{run_idx}.csv",
                                file_label=file_label,
                                input_format=args.input_format,
                                run_index=run_idx,
                                used_lift=used_lift,
                                lift_k=lift_k,
                                backend_input_kind="cnf-exp",
                                timeout_s=args.timeout,
                                used_conversion=True,
                                conversion_format="cnf-exp",
                            )
                        )
                    if "xorricane" in args.solvers:
                        file_rows.append(
                            run_kissat_or_cms(
                                solver_name="xorricane",
                                solver_config=external_solver_config_name("xorricane", "cnf-exp"),
                                backend_input_kind="cnf-exp",
                                bin_path=Path(args.xorricane_bin),
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
                    for kissat_plain in selected_kissat_modes(args):
                        file_rows.append(
                            run_kissat_or_cms(
                                solver_name="kissat",
                                solver_config=external_solver_config_name("kissat", "cnf-exp", kissat_plain=kissat_plain),
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
                                kissat_plain=kissat_plain,
                            )
                        )
                    if "cryptominisat" in args.solvers:
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
    effective_is_xnf = args.input_format == "xnf"
    routes: List[tuple[str, str, str]] = []
    kissat_backend_kind = "cnf"
    kissat_routes = [
        ("kissat", external_solver_config_name("kissat", kissat_backend_kind, kissat_plain=kissat_plain), kissat_backend_kind)
        for kissat_plain in selected_kissat_modes(args)
    ]
    if effective_is_xnf:
        if "xorcle_0_1" in args.solvers:
            routes.append(("xorcle_0_1", "xorcle_0_1_xnf", "xnf"))
        if "xorricane" in args.solvers:
            routes.append(("xorricane", external_solver_config_name("xorricane", "xnf"), "xnf"))
        if "cryptominisat" in args.solvers:
            routes.append(("cryptominisat", "cryptominisat_cnf_xor", "cnf-xor"))
    else:
        if "xorcle_0_1" in args.solvers:
            routes.append(("xorcle_0_1", "xorcle_0_1_cnf", "cnf"))
        if "xorricane" in args.solvers:
            routes.append(("xorricane", external_solver_config_name("xorricane", "cnf"), "cnf"))
        if "cryptominisat" in args.solvers:
            routes.append(("cryptominisat", "cryptominisat_cnf", "cnf"))
    if "2xornado" in args.solvers:
        routes.append(("2xornado", external_solver_config_name("2xornado", "2xnf"), "2xnf"))
    routes.extend(kissat_routes)
    if args.exp_cnf and effective_is_xnf and "xorcle_0_1" in args.solvers:
        routes.append(("xorcle_0_1", "xorcle_0_1_cnf_exp", "cnf-exp"))
    if args.exp_cnf and effective_is_xnf and "xorricane" in args.solvers:
        routes.append(("xorricane", external_solver_config_name("xorricane", "cnf-exp"), "cnf-exp"))
    if args.exp_cnf and effective_is_xnf and "cryptominisat" in args.solvers:
        routes.append(("cryptominisat", "cryptominisat_cnf_exp", "cnf-exp"))
    if args.exp_cnf and effective_is_xnf:
        routes.extend(
            [
                ("kissat", external_solver_config_name("kissat", "cnf-exp", kissat_plain=kissat_plain), "cnf-exp")
                for kissat_plain in selected_kissat_modes(args)
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
    needs_converter = args.input_format == "xnf" or "2xornado" in args.solvers
    if needs_converter and not converter_script.exists():
        print(f"Error: converter script not found: {converter_script}", file=sys.stderr)
        return 2

    raw_rows: List[Dict[str, str]] = []
    fatal_error = False
    with ThreadPoolExecutor(max_workers=args.jobs) as executor:
        future_to_file = {
            executor.submit(benchmark_file, file_path, args, solver_passthrough, converter_script): file_path
            for file_path in input_files
        }
        for future in as_completed(future_to_file):
            file_path = future_to_file[future]
            try:
                raw_rows.extend(future.result())
            except Exception as exc:
                if not args.keep_going:
                    print(f"Fatal error while benchmarking {file_path}: {exc}", file=sys.stderr)
                    executor.shutdown(wait=False, cancel_futures=True)
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

    raw_rows.sort(
        key=lambda row: (
            row.get("file", ""),
            int(row.get("run_index", "0") or 0),
            row.get("solver_config", ""),
            row.get("backend_input_kind", ""),
            row.get("solver", ""),
        )
    )

    summary = summarize_rows(raw_rows)
    print_summary(summary, total_rows=len(raw_rows))

    if args.csv_path:
        write_raw_csv(Path(args.csv_path), raw_rows)
        print(f"\nRaw CSV written to: {args.csv_path}")

    return 0 if not fatal_error else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
