#!/usr/bin/env python3
from __future__ import annotations

import argparse
import sys
from pathlib import Path
from typing import Dict, List, Sequence

import benchmark_core as core
from benchmark_orchestration import ALL_SOLVER_LINES
from benchmark_orchestration import DEFAULT_SOLVER_LINES
from tool_common import repo_root

SOLVER_CHOICES = (
    "xorcle",
    "2xornado",
    "kissat_plain",
    "kissat_default",
    "cryptominisat",
)
XORCLE_PUBLIC_NAME = "Xorcle"


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
    parser.add_argument("-i", "--input-format", required=True, metavar="{cnf,xnf}", help="Input format for the selected files.")
    parser.add_argument(
        "-t",
        "--timeout",
        type=float,
        default=5.0,
        help="Timeout in seconds for all solver runs (default: 5.0)",
    )
    parser.add_argument(
        "--memory-limit-gb",
        type=float,
        default=core.DEFAULT_BENCHMARK_MEMORY_LIMIT_GB,
        help=f"Per-process memory cap in GiB for benchmarked solver runs (default: {core.DEFAULT_BENCHMARK_MEMORY_LIMIT_GB})",
    )
    parser.add_argument("-b", "--solver-bin", default=str(root / "bin" / "xorcle"))
    parser.add_argument("-k", "--kissat-bin", default=str(root / "external" / "kissat"))
    parser.add_argument("-m", "--cms-bin", default=str(root / "external" / "cryptominisat5"))
    parser.add_argument("--2xornado-bin", dest="xornado_bin", default=str(root / "external" / "2xornado"))
    parser.add_argument("--cache-root", default=str(root / "benchmarks" / "cache"))
    parser.add_argument("-c", "--csv", dest="csv_path", default="", help="Optional CSV path for raw benchmark rows")
    parser.add_argument(
        "-e",
        "--exp-cnf",
        action="store_true",
        help=f"For XNF inputs, also benchmark naive no-new-vars exponential CNF encoding (including {XORCLE_PUBLIC_NAME})",
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
        help="Subset of solver routes to execute.",
    )
    parser.add_argument(
        "--saveconverted",
        action="store_true",
        help="Persist converted CNF/CNF-XOR/2-XNF files under tests/converted/<type>/.",
    )
    parser.add_argument(
        "--force-rerun",
        action="store_true",
        help="Ignore cached per-file benchmark results and rerun solver work.",
    )
    args = parser.parse_args(main_argv)

    raw_input_format = str(args.input_format).strip().lower()
    if raw_input_format == "dimacs":
        args.input_format = "cnf"
    elif raw_input_format in {"cnf", "xnf"}:
        args.input_format = raw_input_format
    else:
        parser.error("--input-format must be one of: cnf, xnf")

    if args.jobs < 1:
        parser.error("--jobs must be >= 1")
    if args.timeout <= 0:
        parser.error("--timeout must be > 0")
    if args.memory_limit_gb <= 0:
        parser.error("--memory-limit-gb must be > 0")
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
def main(argv: Sequence[str]) -> int:
    try:
        args, solver_passthrough = parse_args(argv)
        input_files = core.collect_input_files(Path(args.input_path))
    except (SystemExit, FileNotFoundError, ValueError) as exc:
        if isinstance(exc, SystemExit):
            raise
        print(f"Error: {exc}", file=sys.stderr)
        return 2

    converter_script = repo_root() / "tools" / "xnf_convert.py"
    needs_converter = args.input_format == "xnf" or "2xornado" in args.solvers
    cache_root = Path(args.cache_root).resolve()
    if needs_converter and not converter_script.exists():
        print(f"Error: converter script not found: {converter_script}", file=sys.stderr)
        return 2

    raw_rows: List[Dict[str, str]] = []
    fatal_error = False
    try:
        def report_parse_warnings(_file_path: Path, _reused: bool, file_rows: List[Dict[str, str]]) -> None:
            for row in file_rows:
                warning = core.row_parse_issue_message(row)
                if warning:
                    print(f"[parse-warning] {warning}", file=sys.stderr)

        raw_rows = core.ensure_benchmark_file_coverage(
            input_files,
            args,
            solver_passthrough,
            converter_script,
            jobs=args.jobs,
            cache_root=cache_root,
            force_rerun=bool(args.force_rerun),
            progress_callback=report_parse_warnings,
        )
    except Exception as exc:
        fatal_error = True
        for file_path in input_files:
            for solver_name, solver_config, backend_input_kind in core.default_solver_routes_for_args(args):
                row = core.empty_raw_row(
                    str(file_path),
                    args.input_format,
                    solver_name,
                    solver_config,
                    backend_input_kind,
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
            row.get("solver_config", ""),
            row.get("backend_input_kind", ""),
            row.get("solver", ""),
        )
    )

    summary = core.summarize_rows(raw_rows)
    core.print_summary(summary, total_rows=len(raw_rows))

    if args.csv_path:
        core.write_raw_csv(Path(args.csv_path), raw_rows)
        print(f"\nRaw CSV written to: {args.csv_path}")

    return 0 if not fatal_error else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
