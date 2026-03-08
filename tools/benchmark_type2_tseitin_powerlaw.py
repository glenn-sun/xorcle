#!/usr/bin/env python3
from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
import csv
import os
import sys
import time
from collections import defaultdict
from pathlib import Path
from types import SimpleNamespace
from typing import Dict, List

os.environ.setdefault("MPLCONFIGDIR", str(Path("/tmp") / "parity-sat-cpp-mplconfig"))

sys.path.append(str(Path(__file__).resolve().parent))
import run_benchmark as bs
from benchmark_plot_style import (
    apply_common_axes_style,
    create_unified_figure,
    finalize_figure,
    place_right_legend,
    series_style,
)
from benchmark_orchestration import (
    FAMILY_SPECS,
    SOLVER_COLORS,
    SOLVER_DISPLAY,
    TSEITIN_FAMILIES,
    collect_files_by_x,
    eval_power_fit,
    format_progress_line,
    power_fit,
    runtime_for_row,
    should_report_progress,
    solver_line_key,
    timeout_aware_median_runtime,
)

SOLVER = "xorcle_0_1"


def repo_root() -> Path:
    return Path(__file__).resolve().parents[1]


def parse_args() -> argparse.Namespace:
    root = repo_root()
    parser = argparse.ArgumentParser(description="Type 2 benchmark: Tseitin-only, xorcle-only, power-law trendline.")
    parser.add_argument("--tests-root", default=str(root / "tests"))
    parser.add_argument("--output-root", default=str(root / "benchmarks" / "type2"))
    parser.add_argument("--solver-bin", default=str(root / "bin" / "main-release"))
    parser.add_argument("--2xornado-bin", dest="xornado_bin", default=str(root / "external" / "2xornado"))
    parser.add_argument("--xorricane-bin", default=str(root / "external" / "xorricane"))
    parser.add_argument("--kissat-bin", default=str(root / "external" / "kissat"))
    parser.add_argument("--cms-bin", default=str(root / "external" / "cryptominisat5"))
    parser.add_argument("--attempts", type=int, default=1)
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--timeout", type=float, default=5.0)
    parser.add_argument("--saveconverted", action="store_true")
    parser.add_argument("--full", action="store_true", help="Shorthand for --attempts 5 --timeout 60")
    parser.add_argument("--replot-only", action="store_true", help="Regenerate plots from existing CSV files only.")
    parser.add_argument(
        "--families",
        nargs="*",
        choices=list(TSEITIN_FAMILIES),
        default=list(TSEITIN_FAMILIES),
    )
    args = parser.parse_args()

    if args.full:
        args.attempts = 5
        args.timeout = 60.0

    if args.attempts < 1:
        parser.error("--attempts must be >= 1")
    if args.jobs < 1:
        parser.error("--jobs must be >= 1")
    if args.timeout <= 0:
        parser.error("--timeout must be > 0")
    return args


def make_runner_args(*, cli_args: argparse.Namespace, input_format: str, repeat: int) -> SimpleNamespace:
    return SimpleNamespace(
        input_format=input_format,
        repeat=repeat,
        timeout=cli_args.timeout,
        solver_bin=str(Path(cli_args.solver_bin).resolve()),
        kissat_bin=str(Path(cli_args.kissat_bin).resolve()),
        cms_bin=str(Path(cli_args.cms_bin).resolve()),
        xornado_bin=str(Path(cli_args.xornado_bin).resolve()),
        xorricane_bin=str(Path(cli_args.xorricane_bin).resolve()),
        csv_path="",
        kissat_mode="both",
        exp_cnf=False,
        keep_going=True,
        jobs=1,
        solvers=[SOLVER],
        saveconverted=bool(cli_args.saveconverted),
    )


def write_csv(path: Path, rows: List[Dict[str, str]], fieldnames: List[str]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fieldnames)
        w.writeheader()
        w.writerows(rows)


def read_csv_rows(path: Path) -> List[Dict[str, str]]:
    with path.open("r", encoding="utf-8", newline="") as f:
        return list(csv.DictReader(f))


def run_size_task(
    *,
    cli_args: argparse.Namespace,
    input_format: str,
    file_path: Path,
    converter_script: Path,
) -> List[Dict[str, str]]:
    runner_args = make_runner_args(cli_args=cli_args, input_format=input_format, repeat=1)
    return bs.benchmark_file(file_path, runner_args, [], converter_script)


def plot_family(
    *,
    path: Path,
    family: str,
    x_values: List[int],
    x_label: str,
    fit_mode: str,
    points: List[tuple[int, float]],
    median_points: List[tuple[int, float]],
) -> None:
    try:
        import matplotlib

        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ModuleNotFoundError as exc:  # pragma: no cover
        raise SystemExit(f"Error: missing dependency '{exc.name}'. Install matplotlib to run plotting.")

    path.parent.mkdir(parents=True, exist_ok=True)
    fig, ax = create_unified_figure(plt)
    style = series_style(0, color_override=SOLVER_COLORS[SOLVER])
    fit_style = series_style(1, color_override="#222222")
    label = SOLVER_DISPLAY[SOLVER]

    if points:
        pts = sorted(points, key=lambda p: (p[0], p[1]))
        ax.scatter(
            [x for x, _ in pts],
            [y for _, y in pts],
            s=14,
            alpha=0.7,
            color=style.color,
            marker=style.marker,
            label="All non-timeout runs",
        )

    if median_points:
        mp = sorted(median_points, key=lambda p: p[0])
        ax.plot(
            [x for x, _ in mp],
            [y for _, y in mp],
            color=style.color,
            linestyle=style.linestyle,
            linewidth=1.3,
            alpha=0.85,
            label="Median per size",
        )

        fit = power_fit(mp, fit_mode)
        if fit is not None:
            a, p, r2 = fit
            x_lo = min(x for x, _ in mp)
            x_hi = max(x for x, _ in mp)
            if x_lo == x_hi:
                fit_x = [float(x_lo)]
            else:
                step = (x_hi - x_lo) / 199.0
                fit_x = [x_lo + i * step for i in range(200)]
            fit_y = [eval_power_fit(x, a, p, fit_mode) for x in fit_x]
            if fit_mode == "power_on_2powx":
                fit_label = f"Power fit: t ∝ (2^{x_label})^{p:.2f} (R^2={r2:.3f})"
            else:
                fit_label = f"Power fit: t ∝ {x_label}^{p:.2f} (R^2={r2:.3f})"
            ax.plot(
                fit_x,
                fit_y,
                color=fit_style.color,
                linestyle=fit_style.linestyle,
                linewidth=1.3,
                label=fit_label,
            )

    ax.set_yscale("log")
    if family == "tseitin_5_n":
        ax.set_xscale("log")
        ax.set_xticks(x_values)
    apply_common_axes_style(ax, xlabel=x_label, ylabel="Time (s)", title=f"{family} ({label})")
    place_right_legend(ax)
    finalize_figure(fig, path)
    plt.close(fig)


def load_plot_inputs_from_agg(path: Path) -> tuple[str, List[int], str, str, List[tuple[int, float]], List[tuple[int, float]]]:
    rows = read_csv_rows(path)
    if not rows:
        raise RuntimeError(f"{path.name}: no rows found")

    family = rows[0].get("family", "") or path.name.removesuffix(".agg.csv")
    x_label = rows[0].get("x_label", "") or FAMILY_SPECS.get(family, FAMILY_SPECS[TSEITIN_FAMILIES[0]]).x_label
    solver = rows[0].get("solver", "") or SOLVER
    x_values = sorted({int(row["x_value"]) for row in rows if row.get("x_value", "").strip()})
    points: List[tuple[int, float]] = []
    median_points: List[tuple[int, float]] = []

    for row in rows:
        if row.get("solver", "").strip() != solver:
            continue
        x_raw = row.get("x_value", "").strip()
        if not x_raw:
            continue
        x = int(x_raw)
        time_raw = row.get("time_s", "").strip()
        if not time_raw:
            continue
        if row.get("series", "").strip() == "point":
            points.append((x, float(time_raw)))
        elif row.get("series", "").strip() == "median":
            median_points.append((x, float(time_raw)))

    return family, x_values, x_label, solver, points, median_points


def replot_from_existing_csv(*, cli_args: argparse.Namespace, output_root: Path) -> int:
    csv_root = output_root / "csv"
    plots_root = output_root / "plots"
    plots_root.mkdir(parents=True, exist_ok=True)

    plotted = 0
    for family in cli_args.families:
        agg_csv_path = csv_root / f"{family}.agg.csv"
        if not agg_csv_path.is_file():
            print(f"[type2] skipping {family}: missing CSV {agg_csv_path}")
            continue
        csv_family, x_values, x_label, _solver, points, median_points = load_plot_inputs_from_agg(agg_csv_path)
        plot_path = plots_root / f"{csv_family}.png"
        plot_family(
            path=plot_path,
            family=csv_family,
            x_values=x_values,
            x_label=x_label,
            fit_mode=FAMILY_SPECS[csv_family].fit_mode,
            points=points,
            median_points=median_points,
        )
        plotted += 1
        print(f"[type2] replot wrote: {plot_path}")

    if plotted == 0:
        print(f"[type2] no plots regenerated from {csv_root}")
    return 0


def main() -> int:
    args = parse_args()
    root = repo_root()
    tests_root = Path(args.tests_root).resolve()
    output_root = Path(args.output_root).resolve()
    csv_root = output_root / "csv"
    plots_root = output_root / "plots"
    converter_script = root / "tools" / "xnf_convert.py"

    csv_root.mkdir(parents=True, exist_ok=True)
    plots_root.mkdir(parents=True, exist_ok=True)

    if args.replot_only:
        return replot_from_existing_csv(cli_args=args, output_root=output_root)

    total_chunks = sum(len(FAMILY_SPECS[f].x_values) for f in args.families)
    done_chunks = 0
    start_all = time.perf_counter()

    for family in args.families:
        spec = FAMILY_SPECS[family]
        files_by_x = collect_files_by_x(tests_root, spec)
        for x in spec.x_values:
            count = len(files_by_x[x])
            if count < args.attempts:
                raise RuntimeError(
                    f"{family}: need at least {args.attempts} instances for x={x}, found {count}. "
                    "Regenerate tests with tools/generate_xnf_tests.py --attempts ..."
                )

        raw_rows: List[Dict[str, str]] = []
        points: List[tuple[int, float]] = []
        median_by_x: Dict[int, float | None] = {}

        for x in spec.x_values:
            selected_files = files_by_x[x][: args.attempts]
            x_rows: List[Dict[str, str]] = []
            with ThreadPoolExecutor(max_workers=args.jobs) as executor:
                futures = [
                    executor.submit(
                        run_size_task,
                        cli_args=args,
                        input_format=spec.input_format,
                        file_path=file_path,
                        converter_script=converter_script,
                    )
                    for file_path in selected_files
                ]
                for future in as_completed(futures):
                    x_rows.extend(future.result())
            raw_rows.extend(x_rows)

            solver_rows = [row for row in x_rows if solver_line_key(row) == SOLVER]
            median_by_x[x] = timeout_aware_median_runtime(solver_rows)
            for row in solver_rows:
                t = runtime_for_row(row)
                if t is not None:
                    points.append((x, t))

            done_chunks += 1
            if should_report_progress(done_chunks, total_chunks, every=1):
                print(
                    format_progress_line(
                        done=done_chunks,
                        total=total_chunks,
                        start_s=start_all,
                        family=family,
                        x=x,
                    )
                )

        raw_rows.sort(
            key=lambda row: (
                Path(row.get("file", "")).name,
                int(row.get("run_index", "0") or 0),
                row.get("solver_config", ""),
                row.get("backend_input_kind", ""),
                row.get("solver", ""),
            )
        )

        raw_csv_path = csv_root / f"{family}.raw.csv"
        write_csv(raw_csv_path, raw_rows, bs.RAW_CSV_FIELDS)

        agg_rows: List[Dict[str, str]] = []
        for x, t in sorted(points, key=lambda p: (p[0], p[1])):
            agg_rows.append(
                {
                    "family": family,
                    "x_label": spec.x_label,
                    "x_value": str(x),
                    "solver": SOLVER,
                    "series": "point",
                    "time_s": f"{t:.6f}",
                    "status": "",
                }
            )
        median_points: List[tuple[int, float]] = []
        for x in spec.x_values:
            med = median_by_x.get(x)
            if med is not None:
                median_points.append((x, med))
            agg_rows.append(
                {
                    "family": family,
                    "x_label": spec.x_label,
                    "x_value": str(x),
                    "solver": SOLVER,
                    "series": "median",
                    "time_s": "" if med is None else f"{med:.6f}",
                    "status": "TIMEOUT" if med is None else "",
                }
            )

        agg_csv_path = csv_root / f"{family}.agg.csv"
        write_csv(agg_csv_path, agg_rows, ["family", "x_label", "x_value", "solver", "series", "time_s", "status"])

        plot_path = plots_root / f"{family}.png"
        plot_family(
            path=plot_path,
            family=family,
            x_values=list(spec.x_values),
            x_label=spec.x_label,
            fit_mode=spec.fit_mode,
            points=points,
            median_points=median_points,
        )

        print(f"[{family}] wrote: {raw_csv_path}")
        print(f"[{family}] wrote: {agg_csv_path}")
        print(f"[{family}] wrote: {plot_path}")

    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (FileNotFoundError, RuntimeError, ValueError) as exc:
        print(f"Error: {exc}", file=sys.stderr)
        raise SystemExit(2)
