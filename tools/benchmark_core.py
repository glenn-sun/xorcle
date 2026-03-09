#!/usr/bin/env python3
from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
import os
import statistics
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path
from typing import Any, Callable, Dict, List, Optional, Sequence

from tool_common import repo_root
from tool_process import parse_decisions_from_output_details
from tool_process import parse_solver_status_details
from tool_process import parse_solver_status_and_time_details
from tool_process import run_process
from xnf_lib import convert_xnf_to_cnf_naive
from xnf_lib import parse_xnf_file
from xnf_lib import write_cnf

BENCHMARK_SOLVERS = (
    "xorcle",
    "kissat_default",
    "kissat_plain",
    "cryptominisat",
    "2xornado",
)
DEFAULT_BENCHMARK_MEMORY_LIMIT_GB = 8.0

EXECUTION_CACHE_DIRNAME = "executions"
EXECUTION_RESULTS_CSV = "results.csv"
EXECUTION_CONFIG_JSON = "execution_config.json"

_XORCLE_DEFAULTS = {
    "decision_proc": "clause_vsids",
    "clause_vsids": {"learned_base": 1.0, "decay": 0.98, "bump": 1.0},
    "decision_phase": "unsat",
    "clause_selector": "cyclic",
    "compact": {"enabled": True, "interval": 16},
}

RAW_CSV_FIELDS = [
    "file",
    "sample_key",
    "input_format",
    "solver",
    "solver_config",
    "backend_input_kind",
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
    "parse_warning",
    "error_message",
]


def _stable_json_text(data: Any) -> str:
    return json.dumps(data, sort_keys=True, separators=(",", ":"), indent=2)


def config_digest(run_config: Dict[str, Any]) -> str:
    return hashlib.sha256(_stable_json_text(run_config).encode("utf-8")).hexdigest()


def write_run_config(path: Path, run_config: Dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(_stable_json_text(run_config) + "\n", encoding="utf-8")


def read_run_config(path: Path) -> Dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8"))


def run_has_matching_artifacts(
    run_dir: Path,
    run_config: Dict[str, Any],
    required_relpaths: Sequence[str],
) -> bool:
    config_path = run_dir / "run_config.json"
    if not config_path.is_file():
        return False
    try:
        existing_config = read_run_config(config_path)
    except (OSError, json.JSONDecodeError):
        return False
    if existing_config != run_config:
        return False
    return all((run_dir / relpath).is_file() for relpath in required_relpaths)


def collect_input_files(input_path: Path) -> List[Path]:
    if not input_path.exists():
        raise FileNotFoundError(f"Input path does not exist: {input_path}")
    if input_path.is_file():
        return [input_path.resolve()]
    if input_path.is_dir():
        files = sorted(path.resolve() for path in input_path.rglob("*.cnf") if path.is_file())
        if not files:
            raise FileNotFoundError(f"No .cnf files found under directory: {input_path}")
        return files
    raise ValueError(f"Input path is neither file nor directory: {input_path}")


def read_raw_csv(path: Path) -> List[Dict[str, str]]:
    with path.open("r", encoding="utf-8", newline="") as csv_file:
        return list(csv.DictReader(csv_file))


def write_raw_csv(path: Path, rows: List[Dict[str, str]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as csv_file:
        writer = csv.DictWriter(csv_file, fieldnames=RAW_CSV_FIELDS)
        writer.writeheader()
        writer.writerows(rows)


def write_raw_csv_atomic(path: Path, rows: List[Dict[str, str]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(
        mode="w",
        encoding="utf-8",
        newline="",
        dir=path.parent,
        prefix=f".{path.name}.",
        suffix=".tmp",
        delete=False,
    ) as temp_file:
        writer = csv.DictWriter(temp_file, fieldnames=RAW_CSV_FIELDS)
        writer.writeheader()
        writer.writerows(rows)
        temp_path = Path(temp_file.name)
    os.replace(temp_path, path)


def write_json_atomic(path: Path, data: Dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(
        mode="w",
        encoding="utf-8",
        dir=path.parent,
        prefix=f".{path.name}.",
        suffix=".tmp",
        delete=False,
    ) as temp_file:
        temp_file.write(_stable_json_text(data) + "\n")
        temp_path = Path(temp_file.name)
    os.replace(temp_path, path)


def execution_cache_directory(cache_root: Path, execution_config: Dict[str, Any]) -> Path:
    digest = config_digest(execution_config)[:16]
    return cache_root / EXECUTION_CACHE_DIRNAME / digest


def read_execution_rows(cache_root: Path, execution_config: Dict[str, Any]) -> List[Dict[str, str]]:
    cache_dir = execution_cache_directory(cache_root, execution_config)
    config_path = cache_dir / EXECUTION_CONFIG_JSON
    rows_path = cache_dir / EXECUTION_RESULTS_CSV
    if not config_path.is_file() or not rows_path.is_file():
        return []
    existing_config = read_run_config(config_path)
    if existing_config != execution_config:
        return []
    return read_raw_csv(rows_path)


def write_execution_rows(cache_root: Path, execution_config: Dict[str, Any], rows: List[Dict[str, str]]) -> None:
    cache_dir = execution_cache_directory(cache_root, execution_config)
    cache_dir.mkdir(parents=True, exist_ok=True)
    write_json_atomic(cache_dir / EXECUTION_CONFIG_JSON, execution_config)
    write_raw_csv_atomic(cache_dir / EXECUTION_RESULTS_CSV, rows)


def parse_xorcle_csv(csv_path: Path) -> tuple[Optional[str], Optional[float], Optional[int], str, str]:
    if not csv_path.exists():
        return None, None, None, f"missing solver stats CSV: {csv_path}", ""
    try:
        with csv_path.open("r", encoding="utf-8", newline="") as csv_file:
            rows = list(csv.DictReader(csv_file))
    except Exception as exc:
        return None, None, None, f"failed to read solver stats CSV: {exc}", ""
    if not rows:
        return None, None, None, "solver stats CSV was empty", ""

    row = rows[-1]
    status = row.get("result", "").strip().upper() or None
    parsed_time_s = None
    parsed_decisions = None
    parse_warnings: List[str] = []

    raw_time = (row.get("time_s") or "").strip()
    if raw_time:
        try:
            parsed_time_s = float(raw_time)
        except ValueError:
            parse_warnings.append(f"malformed Xorcle time metric in CSV: {raw_time!r}")

    raw_decisions = (row.get("decisions") or "").strip()
    if raw_decisions:
        try:
            parsed_decisions = int(raw_decisions)
        except ValueError:
            parse_warnings.append(f"malformed Xorcle decisions metric in CSV: {raw_decisions!r}")

    return status, parsed_time_s, parsed_decisions, "", "; ".join(parse_warnings)


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
        rel_path = source_resolved.relative_to(tests_root)
    except ValueError:
        rel_path = Path(source_input.name)

    output_path = root / "tests" / "converted" / converted_kind / rel_path
    output_path.parent.mkdir(parents=True, exist_ok=True)
    try:
        output_path.write_bytes(converted_file.read_bytes())
    except Exception as exc:
        return f"failed to persist converted file '{converted_file}' -> '{output_path}': {exc}"
    return None


def normalize_status(status: str) -> str:
    normalized = status.strip().upper()
    if normalized in {"SAT", "UNSAT", "TIMEOUT", "MEMORY", "ERROR"}:
        return normalized
    return "ERROR"


def benchmark_memory_limit_bytes(args: argparse.Namespace) -> int:
    memory_limit_gb = float(getattr(args, "memory_limit_gb", DEFAULT_BENCHMARK_MEMORY_LIMIT_GB))
    return int(memory_limit_gb * 1024 * 1024 * 1024)


def _format_memory_limit_label(memory_limit_bytes: int) -> str:
    gib = 1024 * 1024 * 1024
    if memory_limit_bytes % gib == 0:
        return f"{memory_limit_bytes // gib} GiB"
    mib = 1024 * 1024
    if memory_limit_bytes % mib == 0:
        return f"{memory_limit_bytes // mib} MiB"
    return f"{memory_limit_bytes} bytes"


def _try_parse_float(token: str) -> Optional[float]:
    try:
        return float(token)
    except ValueError:
        return None


def _try_parse_positive_int(token: str) -> Optional[int]:
    if not token or token.startswith("-"):
        return None
    try:
        value = int(token)
    except ValueError:
        return None
    return value if value >= 1 else None


def _normalize_xorcle_passthrough(tokens: Sequence[str]) -> Dict[str, Any]:
    normalized = json.loads(json.dumps(_XORCLE_DEFAULTS))
    extras: List[str] = []
    index = 0
    while index < len(tokens):
        token = tokens[index]
        if token in {"-d", "--decision-proc"} and index + 1 < len(tokens):
            proc = tokens[index + 1]
            if proc == "fixed_literals":
                normalized["decision_proc"] = "fixed_literals"
                normalized["clause_vsids"] = None
                index += 2
                continue
            if proc == "variable_vsids":
                normalized["decision_proc"] = "variable_vsids"
                normalized["clause_vsids"] = None
                index += 2
                continue
            if proc == "clause_vsids":
                config = {"learned_base": 1.0, "decay": 0.98, "bump": 1.0}
                maybe_values = tokens[index + 2:index + 5]
                if len(maybe_values) == 3 and all(_try_parse_float(value) is not None for value in maybe_values):
                    config = {
                        "learned_base": float(maybe_values[0]),
                        "decay": float(maybe_values[1]),
                        "bump": float(maybe_values[2]),
                    }
                    index += 5
                else:
                    index += 2
                normalized["decision_proc"] = "clause_vsids"
                normalized["clause_vsids"] = config
                continue
            if proc == "berkmin":
                normalized["decision_proc"] = "clause_vsids"
                normalized["clause_vsids"] = {"learned_base": 1.0, "decay": 0.5, "bump": 0.0}
                index += 2
                continue
            if proc == "cmtf":
                normalized["decision_proc"] = "clause_vsids"
                normalized["clause_vsids"] = {"learned_base": 1.0, "decay": 0.25, "bump": 1.0}
                index += 2
                continue
        if token in {"-p", "--decision-phase"} and index + 1 < len(tokens):
            normalized["decision_phase"] = tokens[index + 1]
            index += 2
            continue
        if token in {"-c", "--clause-selector"} and index + 1 < len(tokens):
            normalized["clause_selector"] = tokens[index + 1]
            index += 2
            continue
        if token in {"-x", "--compact"} and index + 1 < len(tokens):
            enabled_token = tokens[index + 1].lower()
            if enabled_token in {"on", "true", "1", "yes"}:
                normalized["compact"]["enabled"] = True
            elif enabled_token in {"off", "false", "0", "no"}:
                normalized["compact"]["enabled"] = False
            else:
                extras.append(token)
                index += 1
                continue
            if normalized["compact"]["enabled"] and index + 2 < len(tokens):
                maybe_interval = _try_parse_positive_int(tokens[index + 2])
                if maybe_interval is not None:
                    normalized["compact"]["interval"] = maybe_interval
                    index += 3
                    continue
            index += 2
            continue

        extras.append(token)
        index += 1

    if normalized["decision_proc"] != "clause_vsids":
        normalized["clause_vsids"] = None
    if extras:
        normalized["extra_tokens"] = extras
    return normalized


def normalized_solver_passthrough(args: argparse.Namespace, solver_passthrough: Sequence[str]) -> Dict[str, Any]:
    normalized: Dict[str, Any] = {}
    if "xorcle" in args.solvers:
        normalized["xorcle"] = _normalize_xorcle_passthrough(solver_passthrough)
    elif solver_passthrough:
        normalized["raw"] = list(solver_passthrough)
    return normalized


def empty_raw_row(
    file_label: str,
    input_format: str,
    solver: str,
    solver_config: str,
    backend_input_kind: str,
) -> Dict[str, str]:
    return {
        "file": file_label,
        "sample_key": "",
        "input_format": input_format,
        "solver": solver,
        "solver_config": solver_config,
        "backend_input_kind": backend_input_kind,
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
        "parse_warning": "",
        "error_message": "",
    }


def xorcle_config_name(backend_input_kind: str) -> str:
    if backend_input_kind == "cnf-exp":
        return "xorcle_cnf_exp"
    return f"xorcle_{backend_input_kind}"


def row_has_suppressed_parse_warning(row: Dict[str, str]) -> bool:
    solver_name = (row.get("solver") or "").strip()
    returncode = (row.get("returncode") or "").strip()
    return solver_name == "2xornado" and returncode == "1"


def row_parse_warning_message(row: Dict[str, str]) -> Optional[str]:
    warning = (row.get("parse_warning") or "").strip()
    if not warning or row_has_suppressed_parse_warning(row):
        return None
    solver_config = row.get("solver_config") or row.get("solver") or "<unknown-solver>"
    file_label = row.get("file") or "<unknown-file>"
    return f"{solver_config}: {file_label}: {warning}"


def row_parse_issue_message(row: Dict[str, str]) -> Optional[str]:
    warning_message = row_parse_warning_message(row)
    if warning_message is not None:
        return warning_message
    if row_has_suppressed_parse_warning(row):
        return None
    error_message = (row.get("error_message") or "").strip()
    if not error_message:
        return None
    lowered = error_message.lower()
    if "parse" not in lowered and "malformed" not in lowered:
        return None
    solver_config = row.get("solver_config") or row.get("solver") or "<unknown-solver>"
    file_label = row.get("file") or "<unknown-file>"
    return f"{solver_config}: {file_label}: {error_message}"


def external_solver_config_name(
    solver_name: str,
    backend_input_kind: str,
    kissat_plain: Optional[bool] = None,
) -> str:
    if solver_name == "kissat":
        suffix = "cnf_exp" if backend_input_kind == "cnf-exp" else "cnf"
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
    suffix = backend_input_kind.replace("-", "_")
    return f"{solver_name}_{suffix}"


def selected_kissat_variants(args: argparse.Namespace) -> List[bool]:
    variants: List[bool] = []
    if "kissat_plain" in args.solvers:
        variants.append(True)
    if "kissat_default" in args.solvers:
        variants.append(False)
    return variants


def run_external_solver(
    solver_name: str,
    solver_config: str,
    backend_input_kind: str,
    bin_path: Path,
    input_file: Path,
    timeout_s: float,
    file_label: str,
    input_format: str,
    used_lift: bool,
    lift_k: Optional[int],
    used_conversion: bool,
    conversion_format: str,
    memory_limit_bytes: int,
    kissat_plain: Optional[bool] = None,
) -> Dict[str, str]:
    cmd = [str(bin_path)]
    if solver_name == "kissat" and kissat_plain:
        cmd.append("--plain")
    cmd.append(str(input_file))

    proc = run_process(cmd, timeout_s=timeout_s, memory_limit_bytes=memory_limit_bytes)
    row = empty_raw_row(file_label, input_format, solver_name, solver_config, backend_input_kind)
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
    if proc.memory_limited:
        row["status"] = "MEMORY"
        row["error_message"] = f"memory limit exceeded ({_format_memory_limit_label(memory_limit_bytes)})"
        return row
    if proc.error_message:
        row["status"] = "ERROR"
        row["error_message"] = proc.error_message
        return row

    status_parse = parse_solver_status_details(proc.combined_output)
    if status_parse.status is None:
        row["status"] = "ERROR"
        error_detail = status_parse.error or "could not parse SAT status from output"
        row["error_message"] = f"{error_detail} (rc={proc.returncode})"
        return row

    row["status"] = status_parse.status
    decisions_parse = parse_decisions_from_output_details(proc.combined_output)
    if decisions_parse.value is not None:
        row["parsed_solver_decisions"] = str(decisions_parse.value)
    elif decisions_parse.error:
        row["parse_warning"] = decisions_parse.error
    elif row["status"] in {"SAT", "UNSAT"}:
        row["parse_warning"] = "missing solver decisions metric in output"
    return row


def run_xorcle_solver(
    solver_bin: Path,
    input_file: Path,
    solver_passthrough: Sequence[str],
    stats_csv_path: Path,
    file_label: str,
    input_format: str,
    used_lift: bool,
    lift_k: Optional[int],
    backend_input_kind: str,
    timeout_s: float,
    memory_limit_bytes: int,
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
    proc = run_process(cmd, timeout_s=timeout_s, memory_limit_bytes=memory_limit_bytes)
    row = empty_raw_row(
        file_label,
        input_format,
        "xorcle",
        xorcle_config_name(backend_input_kind),
        backend_input_kind,
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
    if proc.memory_limited:
        row["status"] = "MEMORY"
        row["error_message"] = f"memory limit exceeded ({_format_memory_limit_label(memory_limit_bytes)})"
        return row
    if proc.error_message:
        row["status"] = "ERROR"
        row["error_message"] = proc.error_message
        return row

    status, parsed_time_s, parsed_decisions, csv_error, csv_warning = parse_xorcle_csv(stats_csv_path)
    if status is None:
        fallback_parse = parse_solver_status_and_time_details(proc.combined_output)
        if fallback_parse.status is None:
            row["status"] = "ERROR"
            fallback_error = fallback_parse.status_error or "could not parse solver status from console output"
            error_detail = csv_error or fallback_error
            row["error_message"] = f"{error_detail} (rc={proc.returncode})"
            return row
        status = fallback_parse.status
        parsed_time_s = fallback_parse.time_seconds
        if fallback_parse.time_error:
            csv_warning = "; ".join(filter(None, [csv_warning, fallback_parse.time_error]))

    if parsed_decisions is None:
        decisions_parse = parse_decisions_from_output_details(proc.combined_output)
        parsed_decisions = decisions_parse.value
        if decisions_parse.error:
            csv_warning = "; ".join(filter(None, [csv_warning, decisions_parse.error]))

    row["status"] = normalize_status(status)
    if parsed_time_s is not None:
        row["parsed_solver_time_s"] = f"{parsed_time_s:.6f}"
    if parsed_decisions is not None:
        row["parsed_solver_decisions"] = str(parsed_decisions)
    elif row["status"] in {"SAT", "UNSAT"}:
        csv_warning = "; ".join(filter(None, [csv_warning, "missing solver decisions metric in output"]))
    if csv_warning:
        row["parse_warning"] = csv_warning
    return row


def make_external_error_row(
    file_label: str,
    input_format: str,
    solver_name: str,
    solver_config: str,
    backend_input_kind: str,
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
    wants_xorcle = "xorcle" in args.solvers
    wants_xornado = "2xornado" in args.solvers
    wants_kissat = any(solver in args.solvers for solver in ("kissat_plain", "kissat_default"))
    wants_cms = "cryptominisat" in args.solvers

    with tempfile.TemporaryDirectory(prefix="bench_solvers_") as temp_dir:
        temp_dir_path = Path(temp_dir)
        effective_input_file = file_path
        effective_input_format = args.input_format
        used_lift = False
        lift_k: Optional[int] = None

        kissat_input = effective_input_file
        cms_input = effective_input_file
        xornado_input = temp_dir_path / "2xornado_input.cnf"
        kissat_conversion_error = ""
        cms_conversion_error = ""
        xornado_conversion_error = ""
        kissat_conversion_used = False
        cms_conversion_used = False
        xornado_conversion_used = False
        kissat_conversion_format = ""
        cms_conversion_format = ""
        xornado_conversion_format = "2-xnf"
        exp_cnf_enabled = args.exp_cnf and effective_input_format == "xnf"
        exp_cnf_input = temp_dir_path / "exp_input.cnf"
        exp_cnf_error = ""
        memory_limit_bytes = benchmark_memory_limit_bytes(args)

        if wants_xornado:
            xornado_conversion_error = (
                convert_xnf(converter_script, "2-xnf", effective_input_file, xornado_input) or ""
            )
            xornado_conversion_used = True
            if args.saveconverted and not xornado_conversion_error:
                save_error = persist_converted_file(
                    converted_file=xornado_input,
                    source_input=effective_input_file,
                    converted_kind="2_xnf",
                )
                if save_error:
                    xornado_conversion_error = save_error

        if effective_input_format == "xnf" and wants_kissat:
            kissat_input = temp_dir_path / "kissat_input.cnf"
            kissat_conversion_error = convert_xnf(converter_script, "cnf", effective_input_file, kissat_input) or ""
            kissat_conversion_used = True
            kissat_conversion_format = "cnf"
            if args.saveconverted and not kissat_conversion_error:
                save_error = persist_converted_file(
                    converted_file=kissat_input,
                    source_input=effective_input_file,
                    converted_kind="cnf",
                )
                if save_error:
                    kissat_conversion_error = save_error
        if effective_input_format == "xnf" and wants_cms:
            cms_input = temp_dir_path / "cms_input.cnf"
            cms_conversion_error = convert_xnf(converter_script, "cnf-xor", effective_input_file, cms_input) or ""
            cms_conversion_used = True
            cms_conversion_format = "cnf-xor"
            if args.saveconverted and not cms_conversion_error:
                save_error = persist_converted_file(
                    converted_file=cms_input,
                    source_input=effective_input_file,
                    converted_kind="cnf_xor",
                )
                if save_error:
                    cms_conversion_error = save_error
            if exp_cnf_enabled and (wants_xorcle or wants_kissat or wants_cms):
                try:
                    exp_num_vars, exp_xnf_clauses = parse_xnf_file(str(effective_input_file))
                    exp_cnf_clauses = convert_xnf_to_cnf_naive(exp_num_vars, exp_xnf_clauses)
                    write_cnf(str(exp_cnf_input), exp_num_vars, exp_cnf_clauses)
                    if args.saveconverted:
                        save_error = persist_converted_file(
                            converted_file=exp_cnf_input,
                            source_input=effective_input_file,
                            converted_kind="cnf_exp",
                        )
                        if save_error:
                            exp_cnf_error = save_error
                except Exception as exc:
                    exp_cnf_error = f"naive exponential CNF generation failed: {exc}"

        if wants_xorcle:
            file_rows.append(
                run_xorcle_solver(
                    solver_bin=Path(args.solver_bin),
                    input_file=effective_input_file,
                    solver_passthrough=solver_passthrough,
                    stats_csv_path=temp_dir_path / "xorcle_stats.csv",
                    file_label=file_label,
                    input_format=args.input_format,
                    used_lift=used_lift,
                    lift_k=lift_k,
                    backend_input_kind=effective_input_format,
                    timeout_s=args.timeout,
                    memory_limit_bytes=memory_limit_bytes,
                )
            )

        if wants_xornado:
            xornado_solver_config = external_solver_config_name("2xornado", "2xnf")
            if xornado_conversion_error:
                file_rows.append(
                    make_external_error_row(
                        file_label=file_label,
                        input_format=args.input_format,
                        solver_name="2xornado",
                        solver_config=xornado_solver_config,
                        backend_input_kind="2xnf",
                        timeout_s=args.timeout,
                        used_lift=used_lift,
                        lift_k=lift_k,
                        used_conversion=True,
                        conversion_format=xornado_conversion_format,
                        error_message=xornado_conversion_error,
                    )
                )
            else:
                file_rows.append(
                    run_external_solver(
                        solver_name="2xornado",
                        solver_config=xornado_solver_config,
                        backend_input_kind="2xnf",
                        bin_path=Path(args.xornado_bin),
                        input_file=xornado_input,
                        timeout_s=args.timeout,
                        file_label=file_label,
                        input_format=args.input_format,
                        used_lift=used_lift,
                        lift_k=lift_k,
                        used_conversion=xornado_conversion_used,
                        conversion_format=xornado_conversion_format,
                        memory_limit_bytes=memory_limit_bytes,
                    )
                )

        for kissat_plain in selected_kissat_variants(args):
            kissat_solver_config = external_solver_config_name("kissat", "cnf", kissat_plain=kissat_plain)
            if kissat_conversion_error:
                file_rows.append(
                    make_external_error_row(
                        file_label=file_label,
                        input_format=args.input_format,
                        solver_name="kissat",
                        solver_config=kissat_solver_config,
                        backend_input_kind="cnf",
                        timeout_s=args.timeout,
                        used_lift=used_lift,
                        lift_k=lift_k,
                        used_conversion=True,
                        conversion_format=kissat_conversion_format,
                        error_message=kissat_conversion_error,
                    )
                )
            else:
                file_rows.append(
                    run_external_solver(
                        solver_name="kissat",
                        solver_config=kissat_solver_config,
                        backend_input_kind="cnf",
                        bin_path=Path(args.kissat_bin),
                        input_file=kissat_input,
                        timeout_s=args.timeout,
                        file_label=file_label,
                        input_format=args.input_format,
                        used_lift=used_lift,
                        lift_k=lift_k,
                        used_conversion=kissat_conversion_used,
                        conversion_format=kissat_conversion_format,
                        memory_limit_bytes=memory_limit_bytes,
                        kissat_plain=kissat_plain,
                    )
                )

        if wants_cms:
            cms_backend_kind = "cnf-xor" if effective_input_format == "xnf" else "cnf"
            if cms_conversion_error:
                file_rows.append(
                    make_external_error_row(
                        file_label=file_label,
                        input_format=args.input_format,
                        solver_name="cryptominisat",
                        solver_config=external_solver_config_name("cryptominisat", cms_backend_kind),
                        backend_input_kind=cms_backend_kind,
                        timeout_s=args.timeout,
                        used_lift=used_lift,
                        lift_k=lift_k,
                        used_conversion=bool(cms_conversion_format),
                        conversion_format=cms_conversion_format,
                        error_message=cms_conversion_error,
                    )
                )
            else:
                file_rows.append(
                    run_external_solver(
                        solver_name="cryptominisat",
                        solver_config=external_solver_config_name("cryptominisat", cms_backend_kind),
                        backend_input_kind=cms_backend_kind,
                        bin_path=Path(args.cms_bin),
                        input_file=cms_input,
                        timeout_s=args.timeout,
                        file_label=file_label,
                        input_format=args.input_format,
                        used_lift=used_lift,
                        lift_k=lift_k,
                        used_conversion=cms_conversion_used,
                        conversion_format=cms_conversion_format,
                        memory_limit_bytes=memory_limit_bytes,
                    )
                )

        if exp_cnf_enabled:
            kissat_modes = selected_kissat_variants(args)
            if exp_cnf_error:
                if wants_xorcle:
                    row = empty_raw_row(
                        file_label,
                        args.input_format,
                        "xorcle",
                        xorcle_config_name("cnf-exp"),
                        "cnf-exp",
                    )
                    row["status"] = "ERROR"
                    row["used_lift"] = "true" if used_lift else "false"
                    row["lift_k"] = "" if lift_k is None else str(lift_k)
                    row["used_conversion"] = "true"
                    row["conversion_format"] = "cnf-exp"
                    row["error_message"] = exp_cnf_error
                    file_rows.append(row)

                for kissat_plain in kissat_modes:
                    file_rows.append(
                        make_external_error_row(
                            file_label=file_label,
                            input_format=args.input_format,
                            solver_name="kissat",
                            solver_config=external_solver_config_name("kissat", "cnf-exp", kissat_plain=kissat_plain),
                            backend_input_kind="cnf-exp",
                            timeout_s=args.timeout,
                            used_lift=used_lift,
                            lift_k=lift_k,
                            used_conversion=True,
                            conversion_format="cnf-exp",
                            error_message=exp_cnf_error,
                        )
                    )

                if wants_cms:
                    file_rows.append(
                        make_external_error_row(
                            file_label=file_label,
                            input_format=args.input_format,
                            solver_name="cryptominisat",
                            solver_config=external_solver_config_name("cryptominisat", "cnf-exp"),
                            backend_input_kind="cnf-exp",
                            timeout_s=args.timeout,
                            used_lift=used_lift,
                            lift_k=lift_k,
                            used_conversion=True,
                            conversion_format="cnf-exp",
                            error_message=exp_cnf_error,
                        )
                    )
            else:
                if wants_xorcle:
                    file_rows.append(
                        run_xorcle_solver(
                            solver_bin=Path(args.solver_bin),
                            input_file=exp_cnf_input,
                            solver_passthrough=solver_passthrough,
                            stats_csv_path=temp_dir_path / "xorcle_exp_stats.csv",
                            file_label=file_label,
                            input_format=args.input_format,
                            used_lift=used_lift,
                            lift_k=lift_k,
                            backend_input_kind="cnf-exp",
                            timeout_s=args.timeout,
                            memory_limit_bytes=memory_limit_bytes,
                            used_conversion=True,
                            conversion_format="cnf-exp",
                        )
                    )

                for kissat_plain in kissat_modes:
                    file_rows.append(
                        run_external_solver(
                            solver_name="kissat",
                            solver_config=external_solver_config_name("kissat", "cnf-exp", kissat_plain=kissat_plain),
                            backend_input_kind="cnf-exp",
                            bin_path=Path(args.kissat_bin),
                            input_file=exp_cnf_input,
                            timeout_s=args.timeout,
                            file_label=file_label,
                            input_format=args.input_format,
                            used_lift=used_lift,
                            lift_k=lift_k,
                            used_conversion=True,
                            conversion_format="cnf-exp",
                            memory_limit_bytes=memory_limit_bytes,
                            kissat_plain=kissat_plain,
                        )
                    )

                if wants_cms:
                    file_rows.append(
                        run_external_solver(
                            solver_name="cryptominisat",
                            solver_config=external_solver_config_name("cryptominisat", "cnf-exp"),
                            backend_input_kind="cnf-exp",
                            bin_path=Path(args.cms_bin),
                            input_file=exp_cnf_input,
                            timeout_s=args.timeout,
                            file_label=file_label,
                            input_format=args.input_format,
                            used_lift=used_lift,
                            lift_k=lift_k,
                            used_conversion=True,
                            conversion_format="cnf-exp",
                            memory_limit_bytes=memory_limit_bytes,
                        )
                    )

    return file_rows


def _execution_cache_config(args: argparse.Namespace, solver_passthrough: Sequence[str]) -> Dict[str, Any]:
    config = {
        "solvers": list(args.solvers),
        "input_format": args.input_format,
        "timeout": float(args.timeout),
        "memory_limit_gb": float(getattr(args, "memory_limit_gb", DEFAULT_BENCHMARK_MEMORY_LIMIT_GB)),
        "solver_bin": str(Path(args.solver_bin).resolve()),
        "exp_cnf": bool(args.exp_cnf),
        "normalized_solver_passthrough": normalized_solver_passthrough(args, solver_passthrough),
    }
    if hasattr(args, "kissat_bin"):
        config["kissat_bin"] = str(Path(args.kissat_bin).resolve())
    if hasattr(args, "cms_bin"):
        config["cms_bin"] = str(Path(args.cms_bin).resolve())
    if hasattr(args, "xornado_bin"):
        config["xornado_bin"] = str(Path(args.xornado_bin).resolve())
    return config


def _sort_raw_rows(rows: List[Dict[str, str]]) -> List[Dict[str, str]]:
    return sorted(
        rows,
        key=lambda row: (
            row.get("file", ""),
            row.get("solver_config", ""),
            row.get("backend_input_kind", ""),
            row.get("solver", ""),
        ),
    )


def _stable_cached_status(row: Dict[str, str]) -> bool:
    status = (row.get("status") or "").strip().upper()
    if status in {"SAT", "UNSAT", "TIMEOUT", "MEMORY"}:
        return True
    return status == "ERROR" and (row.get("solver") or "").strip() == "2xornado"


def _expected_route_keys(args: argparse.Namespace) -> set[tuple[str, str]]:
    return {
        (solver_config, backend_input_kind)
        for solver_name, solver_config, backend_input_kind in default_solver_routes_for_args(args)
    }


def _covered_files(
    rows: Sequence[Dict[str, str]],
    *,
    args: argparse.Namespace,
) -> set[str]:
    expected_keys = _expected_route_keys(args)
    grouped: Dict[str, set[tuple[str, str, str]]] = {}
    for row in rows:
        if not _stable_cached_status(row):
            continue
        file_key = str(Path(row.get("file", "")).resolve())
        route_key = (
            row.get("solver_config", ""),
            row.get("backend_input_kind", ""),
        )
        grouped.setdefault(file_key, set()).add(route_key)
    return {file_key for file_key, present_keys in grouped.items() if present_keys == expected_keys}


def _rows_by_file(rows: Sequence[Dict[str, str]]) -> Dict[str, List[Dict[str, str]]]:
    grouped: Dict[str, List[Dict[str, str]]] = {}
    for row in rows:
        grouped.setdefault(str(Path(row.get("file", "")).resolve()), []).append(row)
    return grouped


def _file_is_covered(file_rows: Sequence[Dict[str, str]], *, args: argparse.Namespace) -> bool:
    if not file_rows:
        return False
    expected_keys = _expected_route_keys(args)
    present_keys = {
        (
            row.get("solver_config", ""),
            row.get("backend_input_kind", ""),
        )
        for row in file_rows
        if _stable_cached_status(row)
    }
    return present_keys == expected_keys


def _replace_file_rows(
    rows_by_file: Dict[str, List[Dict[str, str]]],
    *,
    file_key: str,
    new_rows: List[Dict[str, str]],
) -> None:
    rows_by_file[file_key] = list(new_rows)


def ensure_benchmark_file_coverage(
    selected_files: Sequence[Path],
    args: argparse.Namespace,
    solver_passthrough: Sequence[str],
    converter_script: Path,
    *,
    jobs: int,
    cache_root: Path,
    force_rerun: bool = False,
    progress_callback: Optional[Callable[[Path, bool, List[Dict[str, str]]], None]] = None,
) -> List[Dict[str, str]]:
    execution_config = _execution_cache_config(args, solver_passthrough)
    existing_rows = read_execution_rows(cache_root, execution_config)
    selected_file_keys = {str(Path(file_path).resolve()) for file_path in selected_files}
    rows_by_file = _rows_by_file(existing_rows)
    covered_files = {
        file_key for file_key, file_rows in rows_by_file.items()
        if _file_is_covered(file_rows, args=args)
    }
    missing_files = [
        Path(file_key)
        for file_key in sorted(selected_file_keys)
        if force_rerun or file_key not in covered_files
    ]
    missing_file_keys = {str(path.resolve()) for path in missing_files}
    if progress_callback is not None:
        for file_key in sorted(selected_file_keys):
            if file_key not in missing_file_keys:
                progress_callback(Path(file_key), True, list(rows_by_file.get(file_key, [])))
    if not missing_files:
        return [row for row in existing_rows if str(Path(row.get("file", "")).resolve()) in selected_file_keys]

    with ThreadPoolExecutor(max_workers=max(1, int(jobs))) as executor:
        future_map = {
            executor.submit(benchmark_file, file_path, args, solver_passthrough, converter_script): file_path
            for file_path in missing_files
        }
        for future in as_completed(future_map):
            file_path = future_map[future]
            file_key = str(file_path.resolve())
            new_rows = future.result()
            _replace_file_rows(rows_by_file, file_key=file_key, new_rows=new_rows)
            if _file_is_covered(new_rows, args=args):
                covered_files.add(file_key)
            else:
                covered_files.discard(file_key)
            merged_rows = _sort_raw_rows([row for file_rows in rows_by_file.values() for row in file_rows])
            write_execution_rows(cache_root, execution_config, merged_rows)
            if progress_callback is not None:
                progress_callback(file_path, False, list(new_rows))

    merged_rows = _sort_raw_rows([row for file_rows in rows_by_file.values() for row in file_rows])
    return [row for row in merged_rows if str(Path(row.get("file", "")).resolve()) in selected_file_keys]
def summarize_rows(rows: List[Dict[str, str]]) -> Dict[str, Dict[str, object]]:
    grouped: Dict[str, Dict[str, List[Dict[str, str]]]] = {}
    for row in rows:
        solver_key = row.get("solver_config") or row["solver"]
        grouped.setdefault(row["file"], {}).setdefault(solver_key, []).append(row)

    summary: Dict[str, Dict[str, object]] = {}
    for file_label, solver_groups in grouped.items():
        file_summary: Dict[str, object] = {}
        for solver_name, solver_rows in solver_groups.items():
            status_counts: Dict[str, int] = {}
            success_statuses = set()
            success_times: List[float] = []
            for row in solver_rows:
                status = row["status"]
                status_counts[status] = status_counts.get(status, 0) + 1
                if status in {"SAT", "UNSAT"}:
                    success_statuses.add(status)
                    try:
                        success_times.append(float(row["elapsed_wall_s"]))
                    except ValueError:
                        pass

            if len(success_statuses) > 1:
                summary_status = "INCONSISTENT"
            elif "SAT" in success_statuses:
                summary_status = "SAT"
            elif "UNSAT" in success_statuses:
                summary_status = "UNSAT"
            elif status_counts.get("TIMEOUT", 0) > 0:
                summary_status = "TIMEOUT"
            elif status_counts.get("MEMORY", 0) > 0:
                summary_status = "MEMORY"
            else:
                summary_status = "ERROR"

            file_summary[solver_name] = {
                "runs": len(solver_rows),
                "status_counts": status_counts,
                "status": summary_status,
                "median_time_s": statistics.median(success_times) if success_times else None,
                "min_time_s": min(success_times) if success_times else None,
                "inconsistent": len(success_statuses) > 1,
            }
        summary[file_label] = file_summary
    return summary


def _format_float(value: object) -> str:
    if value is None:
        return "-"
    return f"{float(value):.4f}"


def print_summary(summary: Dict[str, Dict[str, object]], total_rows: int) -> None:
    files = sorted(summary.keys())
    any_inconsistent = False
    any_errors = False
    preferred_order = {
        "xorcle_cnf": 0,
        "xorcle_xnf": 0,
        "2xornado_2xnf": 1,
        "xorcle_cnf_exp": 2,
        "kissat_plain_cnf": 3,
        "kissat_default_cnf": 4,
        "cryptominisat_cnf": 5,
        "cryptominisat_cnf_xor": 5,
        "kissat_plain_cnf_exp": 6,
        "kissat_default_cnf_exp": 7,
        "cryptominisat_cnf_exp": 8,
    }

    for file_label in files:
        print(f"\nFile: {file_label}")
        print(
            f"{'solver_config':24} {'status':13} {'median_s':>10} {'best_s':>10} "
            f"{'timeouts':>8} {'memory':>8} {'errors':>6} {'runs':>6}"
        )
        print("-" * 97)
        per_solver = summary[file_label]
        for solver_name in sorted(per_solver.keys(), key=lambda name: (preferred_order.get(name, 99), name)):
            item = per_solver[solver_name]
            counts = item["status_counts"]  # type: ignore[index]
            status = str(item["status"])
            median_s = item["median_time_s"]  # type: ignore[index]
            best_s = item["min_time_s"]  # type: ignore[index]
            timeouts = int(counts.get("TIMEOUT", 0))  # type: ignore[union-attr]
            memory = int(counts.get("MEMORY", 0))  # type: ignore[union-attr]
            errors = int(counts.get("ERROR", 0))  # type: ignore[union-attr]
            runs = int(item["runs"])  # type: ignore[arg-type]
            any_inconsistent = any_inconsistent or bool(item["inconsistent"])  # type: ignore[arg-type]
            any_errors = any_errors or errors > 0
            print(
                f"{solver_name:24} {status:13} "
                f"{_format_float(median_s):>10} {_format_float(best_s):>10} "
                f"{timeouts:8d} {memory:8d} {errors:6d} {runs:6d}"
            )

    print("\nSummary:")
    print(f"  files benchmarked: {len(files)}")
    print(f"  total raw runs: {total_rows}")
    print(f"  inconsistencies detected: {'yes' if any_inconsistent else 'no'}")
    print(f"  errors detected: {'yes' if any_errors else 'no'}")


def default_solver_routes_for_args(args: argparse.Namespace) -> List[tuple[str, str, str]]:
    effective_is_xnf = args.input_format == "xnf"
    routes: List[tuple[str, str, str]] = []
    kissat_backend_kind = "cnf"
    if effective_is_xnf:
        if "xorcle" in args.solvers:
            routes.append(("xorcle", "xorcle_xnf", "xnf"))
        if "cryptominisat" in args.solvers:
            routes.append(("cryptominisat", "cryptominisat_cnf_xor", "cnf-xor"))
    else:
        if "xorcle" in args.solvers:
            routes.append(("xorcle", "xorcle_cnf", "cnf"))
        if "cryptominisat" in args.solvers:
            routes.append(("cryptominisat", "cryptominisat_cnf", "cnf"))
    if "2xornado" in args.solvers:
        routes.append(("2xornado", external_solver_config_name("2xornado", "2xnf"), "2xnf"))
    routes.extend(
        (
            "kissat",
            external_solver_config_name("kissat", kissat_backend_kind, kissat_plain=kissat_plain),
            kissat_backend_kind,
        )
        for kissat_plain in selected_kissat_variants(args)
    )
    if args.exp_cnf and effective_is_xnf and "xorcle" in args.solvers:
        routes.append(("xorcle", "xorcle_cnf_exp", "cnf-exp"))
    if args.exp_cnf and effective_is_xnf and "cryptominisat" in args.solvers:
        routes.append(("cryptominisat", "cryptominisat_cnf_exp", "cnf-exp"))
    if args.exp_cnf and effective_is_xnf:
        routes.extend(
            (
                "kissat",
                external_solver_config_name("kissat", "cnf-exp", kissat_plain=kissat_plain),
                "cnf-exp",
            )
            for kissat_plain in selected_kissat_variants(args)
        )
    return routes
