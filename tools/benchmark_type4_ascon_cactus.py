#!/usr/bin/env python3
from __future__ import annotations

import argparse
import sys
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path
from types import SimpleNamespace
from typing import Dict, List, Sequence

sys.path.append(str(Path(__file__).resolve().parent))
import run_benchmark as rb
from benchmark_plot_style import UNIFIED_DPI, UNIFIED_FONT_SIZE, unified_figure_geometry
from benchmark_orchestration import (
    ALL_SOLVER_LINES,
    SOLVER_DISPLAY,
    SOLVER_TO_BENCH_ARG,
    format_progress_line,
    should_report_progress,
    solver_line_key,
)
from plot_cactus import SeriesData, plot_cactus

ASCON_BUCKETS = ("r2", "r3", "r4")


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
    parser.add_argument(
        "--solvers",
        nargs="*",
        choices=list(ALL_SOLVER_LINES),
        default=None,
        help="Optional solver subset to benchmark/replot. Default: all type-1 solver lines.",
    )
    parser.add_argument(
        "--folders",
        nargs="*",
        choices=list(ASCON_BUCKETS),
        default=None,
        help="Optional Ascon subfolders to benchmark/replot. Selecting any disables the aggregate 'all' plot.",
    )
    parser.add_argument("--replot-only", action="store_true", help="Regenerate plots from existing CSV files only.")
    args = parser.parse_args()

    if args.timeout <= 0:
        parser.error("--timeout must be > 0")
    if args.jobs < 1:
        parser.error("--jobs must be >= 1")

    if not args.replot_only:
        ascon_root = Path(args.ascon_root).resolve()
        if not ascon_root.is_dir():
            raise FileNotFoundError(f"ascon root not found: {ascon_root}")
    return args


def selected_buckets(args: argparse.Namespace) -> tuple[str, ...]:
    return tuple(args.folders) if args.folders else ASCON_BUCKETS


def selected_solver_lines(args: argparse.Namespace) -> tuple[str, ...]:
    return tuple(args.solvers) if args.solvers else tuple(ALL_SOLVER_LINES)


def include_all_plot(args: argparse.Namespace) -> bool:
    return not args.folders


def collect_ascon_xnf_files(ascon_root: Path, buckets: Sequence[str]) -> List[Path]:
    files: List[Path] = []
    for bucket in buckets:
        bucket_dir = ascon_root / bucket
        if not bucket_dir.is_dir():
            raise FileNotFoundError(f"ascon bucket not found: {bucket_dir}")
        files.extend(sorted(p.resolve() for p in bucket_dir.rglob("*.xnf") if p.is_file()))
    if not files:
        raise RuntimeError(f"No .xnf files found under selected buckets in: {ascon_root}")
    return sorted(files)


def make_runner_args(args: argparse.Namespace) -> SimpleNamespace:
    solver_lines = selected_solver_lines(args)
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
        solvers=[SOLVER_TO_BENCH_ARG[s] for s in solver_lines],
        saveconverted=bool(args.saveconverted),
    )


def write_csv(path: Path, rows: Sequence[Dict[str, str]], fieldnames: Sequence[str]) -> None:
    import csv

    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fieldnames)
        w.writeheader()
        w.writerows(rows)


def read_csv_rows(path: Path) -> List[Dict[str, str]]:
    import csv

    with path.open("r", encoding="utf-8", newline="") as f:
        return list(csv.DictReader(f))


def bucket_from_row_file(row_file: str, ascon_root: Path | None) -> str:
    path = Path(row_file).expanduser()
    try:
        resolved = path.resolve()
    except OSError:
        resolved = path

    if ascon_root is not None:
        try:
            rel = resolved.relative_to(ascon_root)
            if rel.parts:
                return rel.parts[0]
        except ValueError:
            pass

    for part in resolved.parts:
        if part in {"r2", "r3", "r4"}:
            return part
    raise RuntimeError(f"could not determine Ascon bucket for row file: {row_file}")


def render_cactus_plot(*, output_path: Path, title: str, series: Sequence[SeriesData]) -> None:
    if not series:
        raise RuntimeError(f"plot generation failed for {output_path.name}: no series provided")
    plot_cactus(
        series,
        output_path.resolve(),
        aggregate_per_file="median",
        include_timeouts=False,
        line_mode="line",
        black_white=False,
        title=title,
        xlabel="Instances solved",
        ylabel="Runtime (s)",
        x_scale="linear",
        y_scale="log",
        x_ticks=[],
        y_ticks=[],
        font_size=UNIFIED_FONT_SIZE,
        figsize=unified_figure_geometry()[0],
        dpi=UNIFIED_DPI,
        show_counts=True,
    )


def build_bucket_rows(rows: Sequence[Dict[str, str]], ascon_root: Path | None, buckets: Sequence[str]) -> Dict[str, List[Dict[str, str]]]:
    bucket_rows: Dict[str, List[Dict[str, str]]] = {bucket: [] for bucket in buckets}
    for row in rows:
        bucket = bucket_from_row_file(row.get("file", ""), ascon_root)
        if bucket in bucket_rows:
            bucket_rows[bucket].append(row)
    return bucket_rows


def series_for_bucket(rows: Sequence[Dict[str, str]], bucket: str, solver_lines: Sequence[str]) -> List[SeriesData]:
    series: List[SeriesData] = []
    unknown_rows = [row for row in rows if solver_line_key(row) is None]
    if unknown_rows:
        raise RuntimeError(
            f"{bucket}: found {len(unknown_rows)} rows with unmapped solver/config, e.g. "
            f"solver={unknown_rows[0].get('solver','')} config={unknown_rows[0].get('solver_config','')}"
        )

    for solver in solver_lines:
        solver_rows = [row for row in rows if solver_line_key(row) == solver]
        if not solver_rows:
            continue
        series.append(SeriesData(label=SOLVER_DISPLAY[solver], rows=solver_rows))
    return series


def replot_from_existing_csv(*, args: argparse.Namespace, output_root: Path, ascon_root: Path | None) -> int:
    csv_root = output_root / "csv"
    plots_root = output_root / "plots"
    plots_root.mkdir(parents=True, exist_ok=True)
    buckets = selected_buckets(args)
    solver_lines = selected_solver_lines(args)
    all_enabled = include_all_plot(args)

    all_raw_csv = csv_root / "all.raw.csv"
    if not all_raw_csv.is_file():
        print(f"[type4] skipping replot: missing CSV {all_raw_csv}")
        return 0

    rows = read_csv_rows(all_raw_csv)
    if not rows:
        print(f"[type4] skipping replot: no rows in {all_raw_csv}")
        return 0

    bucket_rows = build_bucket_rows(rows, ascon_root, buckets)
    plotted = 0
    if all_enabled:
        all_series = series_for_bucket(rows, "all", solver_lines)
        if all_series:
            all_plot_path = plots_root / "all.png"
            render_cactus_plot(output_path=all_plot_path, title="Type 4 Ascon XNF Cactus (all)", series=all_series)
            plotted += 1
            print(f"[type4] replot wrote {all_plot_path}")
        else:
            print(f"[type4] skipping all: no mapped solver series")
    else:
        print(f"[type4] skipping all plot because --folders was provided: {', '.join(buckets)}")

    for bucket in buckets:
        current_rows = bucket_rows[bucket]
        if not current_rows:
            print(f"[type4] skipping {bucket}: no rows in {all_raw_csv}")
            continue
        series = series_for_bucket(current_rows, bucket, solver_lines)
        if not series:
            print(f"[type4] skipping {bucket}: no mapped solver series")
            continue
        plot_path = plots_root / f"{bucket}.png"
        render_cactus_plot(output_path=plot_path, title=f"Type 4 Ascon XNF Cactus ({bucket})", series=series)
        plotted += 1
        print(f"[type4] replot wrote {plot_path}")

    if plotted == 0:
        print(f"[type4] no plots regenerated from {csv_root}")
    return 0


def main() -> int:
    args = parse_args()
    root = repo_root()
    ascon_root = Path(args.ascon_root).resolve()
    output_root = Path(args.output_root).resolve()
    csv_root = output_root / "csv"
    plots_root = output_root / "plots"
    converter_script = root / "tools" / "xnf_convert.py"
    buckets = selected_buckets(args)
    solver_lines = selected_solver_lines(args)
    all_enabled = include_all_plot(args)

    if args.replot_only:
        return replot_from_existing_csv(args=args, output_root=output_root, ascon_root=ascon_root if ascon_root.is_dir() else None)

    if not converter_script.is_file():
        raise FileNotFoundError(f"converter script not found: {converter_script}")

    files = collect_ascon_xnf_files(ascon_root, buckets)
    total_files = len(files)
    progress_every = max(1, total_files // 40)
    start_all = time.perf_counter()
    runner_args = make_runner_args(args)

    rows: List[Dict[str, str]] = []
    print(f"[type4] benchmarking {total_files} .xnf files under {ascon_root} for buckets: {', '.join(buckets)}")
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

    bucket_rows = build_bucket_rows(rows, ascon_root, buckets)

    for bucket in buckets:
        if not bucket_rows[bucket]:
            raise RuntimeError(f"No benchmark rows for bucket '{bucket}'")

    if all_enabled:
        all_series = series_for_bucket(rows, "all", solver_lines)
        all_plot_path = plots_root / "all.png"
        render_cactus_plot(output_path=all_plot_path, title="Type 4 Ascon XNF Cactus (all)", series=all_series)
        print(f"[type4] wrote {all_plot_path}")
    else:
        print(f"[type4] skipped all plot because --folders was provided: {', '.join(buckets)}")

    for bucket in buckets:
        current_rows = bucket_rows[bucket]
        bucket_csv_dir = csv_root / bucket
        series = series_for_bucket(current_rows, bucket, solver_lines)
        for solver in solver_lines:
            solver_rows = [row for row in current_rows if solver_line_key(row) == solver]
            if not solver_rows:
                raise RuntimeError(f"{bucket}: no rows for solver line '{solver}'")
            solver_csv = bucket_csv_dir / f"{solver}.raw.csv"
            write_csv(solver_csv, solver_rows, rb.RAW_CSV_FIELDS)

        plot_path = plots_root / f"{bucket}.png"
        render_cactus_plot(output_path=plot_path, title=f"Type 4 Ascon XNF Cactus ({bucket})", series=series)
        print(f"[type4] wrote {plot_path}")

    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (FileNotFoundError, RuntimeError, ValueError) as exc:
        print(f"Error: {exc}", file=sys.stderr)
        raise SystemExit(2)
