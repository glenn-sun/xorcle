#!/usr/bin/env python3
from __future__ import annotations

import argparse
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path
from types import SimpleNamespace
from typing import Dict, List, Sequence, Tuple

sys.path.append(str(Path(__file__).resolve().parent))
import run_benchmark as rb
from benchmark_orchestration import (
    ALL_SOLVER_LINES,
    SOLVER_DISPLAY,
    SOLVER_TO_BENCH_ARG,
    format_progress_line,
    should_report_progress,
    solver_line_key,
)


def repo_root() -> Path:
    return Path(__file__).resolve().parents[1]


def parse_args() -> argparse.Namespace:
    root = repo_root()
    parser = argparse.ArgumentParser(description="Type 4 benchmark: Ascon XNF cactus across type-1 solvers.")
    parser.add_argument("--ascon-root", default=str(root / "ascon-tests"))
    parser.add_argument("--output-root", default=str(root / "benchmarks" / "type4"))
    parser.add_argument("--solver-bin", default=str(root / "bin" / "main-release"))
    parser.add_argument("--2xornado-bin", dest="xornado_bin", default=str(root / "external" / "2xornado"))
    parser.add_argument("--xorricane-bin", default=str(root / "external" / "xorricane"))
    parser.add_argument("--kissat-bin", default=str(root / "external" / "kissat"))
    parser.add_argument("--cms-bin", default=str(root / "external" / "cryptominisat5"))
    parser.add_argument("--timeout", type=float, default=5.0)
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--saveconverted", action="store_true")
    args = parser.parse_args()

    if args.timeout <= 0:
        parser.error("--timeout must be > 0")
    if args.jobs < 1:
        parser.error("--jobs must be >= 1")

    ascon_root = Path(args.ascon_root).resolve()
    if not ascon_root.is_dir():
        raise FileNotFoundError(f"ascon root not found: {ascon_root}")
    return args


def collect_ascon_xnf_files(ascon_root: Path) -> List[Path]:
    files = sorted(p.resolve() for p in ascon_root.rglob("*.xnf") if p.is_file())
    if not files:
        raise RuntimeError(f"No .xnf files found under: {ascon_root}")
    return files


def make_runner_args(args: argparse.Namespace) -> SimpleNamespace:
    return SimpleNamespace(
        input_format="xnf",
        repeat=1,
        timeout=args.timeout,
        solver_bin=str(Path(args.solver_bin).resolve()),
        kissat_bin=str(Path(args.kissat_bin).resolve()),
        cms_bin=str(Path(args.cms_bin).resolve()),
        xornado_bin=str(Path(args.xornado_bin).resolve()),
        xorricane_bin=str(Path(args.xorricane_bin).resolve()),
        csv_path="",
        kissat_mode="both",
        exp_cnf=False,
        keep_going=True,
        jobs=1,
        solvers=[SOLVER_TO_BENCH_ARG[s] for s in ALL_SOLVER_LINES],
        saveconverted=bool(args.saveconverted),
    )


def write_csv(path: Path, rows: Sequence[Dict[str, str]], fieldnames: Sequence[str]) -> None:
    import csv

    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fieldnames)
        w.writeheader()
        w.writerows(rows)


def bucket_from_row_file(row_file: str, ascon_root: Path) -> str:
    rel = Path(row_file).resolve().relative_to(ascon_root)
    if not rel.parts:
        raise RuntimeError(f"row file path has no relative parts under ascon root: {row_file}")
    return rel.parts[0]


def generate_cactus_plot(*, plot_script: Path, output_path: Path, title: str, inputs: Sequence[Tuple[str, Path]]) -> None:
    output_path.parent.mkdir(parents=True, exist_ok=True)
    cmd = [sys.executable, str(plot_script)]
    for label, csv_path in inputs:
        cmd.extend(["--input", f"{csv_path}::{label}"])
    cmd.extend(
        [
            "--aggregate-per-file",
            "median",
            "--title",
            title,
            "--output",
            str(output_path),
        ]
    )
    cp = subprocess.run(cmd, capture_output=True, text=True)
    if cp.returncode != 0:
        tail = (cp.stderr or cp.stdout or "")[-800:]
        raise RuntimeError(f"plot generation failed for {output_path.name}: {tail}")


def main() -> int:
    args = parse_args()
    root = repo_root()
    ascon_root = Path(args.ascon_root).resolve()
    output_root = Path(args.output_root).resolve()
    csv_root = output_root / "csv"
    plots_root = output_root / "plots"
    converter_script = root / "tools" / "xnf_convert.py"
    plot_script = root / "tools" / "plot_cactus.py"

    if not converter_script.is_file():
        raise FileNotFoundError(f"converter script not found: {converter_script}")
    if not plot_script.is_file():
        raise FileNotFoundError(f"plot script not found: {plot_script}")

    files = collect_ascon_xnf_files(ascon_root)
    total_files = len(files)
    progress_every = max(1, total_files // 40)
    start_all = time.perf_counter()
    runner_args = make_runner_args(args)

    rows: List[Dict[str, str]] = []
    print(f"[type4] benchmarking {total_files} .xnf files under {ascon_root}")
    with ThreadPoolExecutor(max_workers=args.jobs) as executor:
        future_to_file = {
            executor.submit(rb.benchmark_file, file_path, runner_args, [], converter_script): file_path
            for file_path in files
        }
        done = 0
        for future in as_completed(future_to_file):
            file_path = future_to_file[future]
            done += 1
            try:
                rows.extend(future.result())
            except Exception as exc:
                raise RuntimeError(f"benchmark failure for {file_path}: {exc}") from exc
            if should_report_progress(done, total_files, progress_every):
                print(
                    format_progress_line(
                        done=done,
                        total=total_files,
                        start_s=start_all,
                        phase="benchmark",
                    )
                )

    if not rows:
        raise RuntimeError("No benchmark rows produced")

    rows.sort(
        key=lambda row: (
            row.get("file", ""),
            int(row.get("run_index", "0") or 0),
            row.get("solver_config", ""),
            row.get("backend_input_kind", ""),
            row.get("solver", ""),
        )
    )

    all_raw_csv = csv_root / "all.raw.csv"
    write_csv(all_raw_csv, rows, rb.RAW_CSV_FIELDS)
    print(f"[type4] wrote {all_raw_csv}")

    bucket_rows: Dict[str, List[Dict[str, str]]] = {
        "all": list(rows),
        "r2": [],
        "r3": [],
        "r4": [],
    }
    for row in rows:
        bucket = bucket_from_row_file(row.get("file", ""), ascon_root)
        if bucket in bucket_rows and bucket != "all":
            bucket_rows[bucket].append(row)

    for bucket in ("r2", "r3", "r4"):
        if not bucket_rows[bucket]:
            raise RuntimeError(f"No benchmark rows for bucket '{bucket}'")

    for bucket in ("all", "r2", "r3", "r4"):
        current_rows = bucket_rows[bucket]
        bucket_csv_dir = csv_root / bucket
        plot_inputs: List[Tuple[str, Path]] = []
        unknown_rows = [r for r in current_rows if solver_line_key(r) is None]
        if unknown_rows:
            raise RuntimeError(
                f"{bucket}: found {len(unknown_rows)} rows with unmapped solver/config, e.g. "
                f"solver={unknown_rows[0].get('solver','')} config={unknown_rows[0].get('solver_config','')}"
            )

        for solver in ALL_SOLVER_LINES:
            solver_rows = [row for row in current_rows if solver_line_key(row) == solver]
            if not solver_rows:
                raise RuntimeError(f"{bucket}: no rows for solver line '{solver}'")
            solver_csv = bucket_csv_dir / f"{solver}.raw.csv"
            write_csv(solver_csv, solver_rows, rb.RAW_CSV_FIELDS)
            plot_inputs.append((SOLVER_DISPLAY[solver], solver_csv))

        plot_path = plots_root / f"{bucket}.png"
        generate_cactus_plot(
            plot_script=plot_script,
            output_path=plot_path,
            title=f"Type 4 Ascon XNF Cactus ({bucket})",
            inputs=plot_inputs,
        )
        print(f"[type4] wrote {plot_path}")

    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (FileNotFoundError, RuntimeError, ValueError) as exc:
        print(f"Error: {exc}", file=sys.stderr)
        raise SystemExit(2)
