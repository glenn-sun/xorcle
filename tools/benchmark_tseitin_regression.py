#!/usr/bin/env python3
from __future__ import annotations

import argparse
import csv
import sys
import time
from pathlib import Path
from types import SimpleNamespace
from typing import Dict, List, Optional, Sequence

import benchmark_core as bc
from benchmark_orchestration import SOLVER_COLORS
from benchmark_orchestration import SOLVER_DISPLAY
from benchmark_orchestration import TYPE2_VIEW_SPECS
from benchmark_orchestration import eval_power_fit
from benchmark_orchestration import format_progress_line
from benchmark_orchestration import load_type2_view_instances
from benchmark_orchestration import power_fit
from benchmark_orchestration import sample_instances
from benchmark_orchestration import should_report_progress
from benchmark_plot_style import PAPER_LINE_WIDTH, PAPER_MARKER_SIZE, UNIFIED_DPI, UNIFIED_FONT_SIZE
from benchmark_plot_style import apply_common_axes_style
from benchmark_plot_style import apply_log_tick_format
from benchmark_plot_style import paper_legend_style
from benchmark_plot_style import paper_layout_axes_rect
from benchmark_plot_style import paper_layout_figure_size
from benchmark_plot_style import paper_layout_legend_anchor
from benchmark_plot_style import series_style
from benchmark_plot_style import tseitin_regression_layout
from benchmark_plot_style import tseitin_visual_style
from tool_common import repo_root

BENCHMARK_KIND = "tseitin_regression"
RAW_CSV_NAME = "tseitin.raw.csv"
POINTS_CSV_NAME = "tseitin_points.csv"
PLOT_NAME = "tseitin_regression.png"
SOLVER = "xorcle"
VIEW_ORDER = ("tseitin_4_n", "tseitin_k_20")


def import_matplotlib_pyplot():
    try:
        import matplotlib

        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ModuleNotFoundError as exc:  # pragma: no cover
        raise SystemExit(f"Error: missing dependency '{exc.name}'. Install matplotlib to run plotting.")
    return plt


def parse_count(value: int) -> Optional[int]:
    return None if value <= 0 else value


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    root = repo_root()
    parser = argparse.ArgumentParser(description="Tseitin regression plots for Xorcle.")
    parser.add_argument("--tests-root", default=str(root / "tests" / "generated"))
    parser.add_argument("--output-root", default=str(root / "benchmarks" / "tseitin_regression"))
    parser.add_argument("--cache-root", default=str(root / "benchmarks" / "cache"))
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--timeout", type=float, default=5.0)
    parser.add_argument("--memory-limit-gb", type=float, default=bc.DEFAULT_BENCHMARK_MEMORY_LIMIT_GB)
    parser.add_argument("--instances-per-x", type=int, default=0, help="0 means all instances for each x-value.")
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--saveconverted", action="store_true")
    parser.add_argument("--force-rerun", action="store_true")
    parser.add_argument("--replot-only", action="store_true")
    parser.add_argument("--solver-bin", default=str(root / "bin" / "xorcle"))
    args = parser.parse_args(argv)

    if args.jobs < 1:
        parser.error("--jobs must be >= 1")
    if args.timeout <= 0:
        parser.error("--timeout must be > 0")
    if args.memory_limit_gb <= 0:
        parser.error("--memory-limit-gb must be > 0")
    return args


def make_runner_args(args: argparse.Namespace, *, input_format: str) -> SimpleNamespace:
    return SimpleNamespace(
        input_format=input_format,
        timeout=args.timeout,
        memory_limit_gb=args.memory_limit_gb,
        solver_bin=str(Path(args.solver_bin).resolve()),
        csv_path="",
        exp_cnf=False,
        solvers=[SOLVER],
        saveconverted=bool(args.saveconverted),
    )


def run_config_for_args(args: argparse.Namespace, generated_root: Path) -> Dict[str, object]:
    return {
        "benchmark_kind": BENCHMARK_KIND,
        "tests_root": str(generated_root),
        "views": list(VIEW_ORDER),
        "instances_per_x": parse_count(args.instances_per_x),
        "seed": int(args.seed),
        "timeout": float(args.timeout),
        "memory_limit_gb": float(args.memory_limit_gb),
        "solver_bin": str(Path(args.solver_bin).resolve()),
        "saveconverted": bool(args.saveconverted),
        "solvers": [SOLVER],
    }


def write_points_csv(path: Path, rows: Sequence[Dict[str, str]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as csv_file:
        writer = csv.DictWriter(
            csv_file,
            fieldnames=["view", "x_value", "x_label", "fit_mode", "file", "time_s"],
        )
        writer.writeheader()
        writer.writerows(rows)


def read_points_csv(path: Path) -> List[Dict[str, str]]:
    with path.open("r", encoding="utf-8", newline="") as csv_file:
        return list(csv.DictReader(csv_file))


def fit_legend_label(*, x_label: str, fit_mode: str, a: float, p: float) -> str:
    if fit_mode == "power_on_2powx":
        return f"T ∝ (2^{x_label})^{p:.2f}"
    return f"T ∝ {x_label}^{p:.2f}"


def build_plot_data(point_rows: Sequence[Dict[str, str]]) -> Dict[str, Dict[str, object]]:
    plot_data: Dict[str, Dict[str, object]] = {}
    for view_name in VIEW_ORDER:
        view_spec = TYPE2_VIEW_SPECS[view_name]
        rows = [row for row in point_rows if row.get("view") == view_name]
        points = [(int(row["x_value"]), float(row["time_s"])) for row in rows]
        plot_data[view_name] = {
            "view": view_spec,
            "points": points,
        }
    return plot_data


def render_plot(point_rows: Sequence[Dict[str, str]], output_path: Path) -> None:
    plt = import_matplotlib_pyplot()
    from matplotlib.ticker import NullLocator

    layout = tseitin_regression_layout()
    legend_style = paper_legend_style()
    visual_style = tseitin_visual_style()
    figure = plt.figure(figsize=paper_layout_figure_size(layout))
    axes = {
        view_name: figure.add_axes(paper_layout_axes_rect(layout, row=0, col=index))
        for index, view_name in enumerate(VIEW_ORDER)
    }
    plot_data = build_plot_data(point_rows)
    point_style = series_style(0, color_override=SOLVER_COLORS[SOLVER])
    fit_styles = {
        "tseitin_4_n": visual_style.vertex_fit,
        "tseitin_k_20": visual_style.degree_fit,
    }
    fit_labels = {
        "tseitin_4_n": "Fit (vary vertices)",
        "tseitin_k_20": "Fit (vary degree)",
    }

    for view_name in VIEW_ORDER:
        axis = axes[view_name]
        view_spec = plot_data[view_name]["view"]  # type: ignore[index]
        points = sorted(plot_data[view_name]["points"])  # type: ignore[index]
        if points:
            axis.scatter(
                [x for x, _ in points],
                [y for _, y in points],
                s=PAPER_MARKER_SIZE,
                alpha=visual_style.scatter_alpha,
                color=point_style.color,
                marker=point_style.marker,
            )
            fit = power_fit(points, view_spec.fit_mode)
            if fit is not None:
                a, p, _r2 = fit
                fit_labels[view_name] = fit_legend_label(
                    x_label=view_spec.x_label,
                    fit_mode=view_spec.fit_mode,
                    a=a,
                    p=p,
                )
                x_lo = min(x for x, _ in points)
                x_hi = max(x for x, _ in points)
                fit_x = [float(x_lo)] if x_lo == x_hi else [x_lo + ((x_hi - x_lo) / 199.0) * i for i in range(200)]
                fit_y = [eval_power_fit(x, a, p, view_spec.fit_mode) for x in fit_x]
                style = fit_styles[view_name]
                axis.plot(
                    fit_x,
                    fit_y,
                    color=style.color,
                    linestyle=style.linestyle,
                    linewidth=PAPER_LINE_WIDTH,
                )
        axis.set_yscale("log")
        if view_name == "tseitin_4_n":
            axis.set_xscale("log")
            axis.set_xticks(list(view_spec.x_values))
            axis.set_xticklabels([str(value) for value in view_spec.x_values])
        else:
            axis.set_xticks(list(view_spec.x_values))
        apply_common_axes_style(
            axis,
            xlabel=view_spec.x_label,
            ylabel="Time (s)",
            title="Tseitin on n-vertex 4-regular graphs" if view_name == "tseitin_4_n" else "Tseitin on 20-vertex k-regular graphs",
            font_size=UNIFIED_FONT_SIZE,
        )
        apply_log_tick_format(
            axis,
            x_scale="linear" if view_name == "tseitin_4_n" else "linear",
            y_scale="log",
        )
        axis.minorticks_off()
        axis.xaxis.set_minor_locator(NullLocator())
        axis.yaxis.set_minor_locator(NullLocator())
        axis.grid(False, which="minor")

    legend_handles = [
        plt.Line2D([], [], linestyle="none", marker=point_style.marker, color=point_style.color, label="One test file"),
        plt.Line2D([], [], linestyle=fit_styles["tseitin_4_n"].linestyle, color=fit_styles["tseitin_4_n"].color, linewidth=PAPER_LINE_WIDTH, label=fit_labels["tseitin_4_n"]),
        plt.Line2D([], [], linestyle=fit_styles["tseitin_k_20"].linestyle, color=fit_styles["tseitin_k_20"].color, linewidth=PAPER_LINE_WIDTH, label=fit_labels["tseitin_k_20"]),
    ]
    figure.legend(
        legend_handles,
        [handle.get_label() for handle in legend_handles],
        fontsize=UNIFIED_FONT_SIZE,
        frameon=False,
        loc="center left",
        bbox_to_anchor=paper_layout_legend_anchor(layout),
        borderaxespad=legend_style.borderaxespad,
        handleheight=legend_style.handleheight,
        labelspacing=legend_style.labelspacing,
    )

    output_path.parent.mkdir(parents=True, exist_ok=True)
    figure.savefig(output_path, dpi=UNIFIED_DPI)
    plt.close(figure)


def execute_benchmark(args: argparse.Namespace, *, generated_root: Path, run_dir: Path, cache_root: Path) -> tuple[List[Dict[str, str]], List[Dict[str, str]]]:
    converter_script = repo_root() / "tools" / "xnf_convert.py"
    cap = parse_count(args.instances_per_x)
    all_raw_rows: List[Dict[str, str]] = []
    point_rows: List[Dict[str, str]] = []
    total_tasks = sum(
        min(len(records), cap) if cap is not None else len(records)
        for view_name in VIEW_ORDER
        for records in load_type2_view_instances(generated_root, view_name)[1].values()
    )
    progress_every = max(1, total_tasks // 40)
    progress_done = 0
    start_s = time.perf_counter()

    for view_name in VIEW_ORDER:
        view_spec, records_by_x = load_type2_view_instances(generated_root, view_name)
        for x_value in view_spec.x_values:
            records = list(records_by_x[x_value])
            if not records:
                raise RuntimeError(f"{view_name}: no Tseitin instances found for x={x_value}")
            ordered_records = sample_instances(
                records,
                count=len(records),
                seed=args.seed,
                cohort_id=f"{view_name}:{x_value}",
            )
            if cap is not None:
                ordered_records = ordered_records[:cap]

            def report_progress(file_path: Path, reused: bool, file_rows: List[Dict[str, str]]) -> None:
                nonlocal progress_done
                progress_done += 1
                for row in file_rows:
                    warning = bc.row_parse_issue_message(row)
                    if warning:
                        print(f"[parse-warning] {warning}", file=sys.stderr)
                if should_report_progress(progress_done, total_tasks, progress_every):
                    print(
                        format_progress_line(
                            done=progress_done,
                            total=total_tasks,
                            start_s=start_s,
                            view=view_name,
                            x=x_value,
                            file=file_path.name,
                            reused="yes" if reused else "no",
                        )
                    )

            rows = bc.ensure_benchmark_file_coverage(
                [record.file for record in ordered_records],
                make_runner_args(args, input_format=ordered_records[0].input_format),
                [],
                converter_script,
                jobs=args.jobs,
                cache_root=cache_root,
                force_rerun=bool(args.force_rerun),
                progress_callback=report_progress,
            )
            rows_by_file: Dict[str, List[Dict[str, str]]] = {}
            for row in rows:
                rows_by_file.setdefault(str(Path(row["file"]).resolve()), []).append(row)
            all_raw_rows.extend(rows)
            for record in ordered_records:
                record_rows = rows_by_file.get(str(record.file.resolve()), [])
                if not record_rows:
                    continue
                first_row = record_rows[0]
                if (first_row.get("status") or "").strip().upper() not in {"SAT", "UNSAT"}:
                    continue
                runtime_seconds: Optional[float] = None
                raw_reported_time = (first_row.get("parsed_solver_time_s") or "").strip()
                if raw_reported_time:
                    try:
                        runtime_seconds = float(raw_reported_time)
                    except (TypeError, ValueError):
                        runtime_seconds = None
                if runtime_seconds is None:
                    try:
                        runtime_seconds = float(first_row.get("elapsed_wall_s", ""))
                    except (TypeError, ValueError):
                        continue
                if runtime_seconds > 0.0:
                    point_rows.append(
                        {
                            "view": view_name,
                            "x_value": str(x_value),
                            "x_label": view_spec.x_label,
                            "fit_mode": view_spec.fit_mode,
                            "file": str(record.file),
                            "time_s": f"{runtime_seconds:.6f}",
                        }
                    )

    all_raw_rows.sort(
        key=lambda row: (
            row.get("file", ""),
            row.get("solver_config", ""),
            row.get("backend_input_kind", ""),
            row.get("solver", ""),
        )
    )
    point_rows.sort(key=lambda row: (row["view"], int(row["x_value"]), row["file"]))
    bc.write_raw_csv(run_dir / RAW_CSV_NAME, all_raw_rows)
    write_points_csv(run_dir / POINTS_CSV_NAME, point_rows)
    return all_raw_rows, point_rows


def main(argv: Sequence[str]) -> int:
    args = parse_args(argv)
    generated_root = Path(args.tests_root).resolve()
    output_root = Path(args.output_root).resolve()
    cache_root = Path(args.cache_root).resolve()
    run_config = run_config_for_args(args, generated_root)
    plot_path = output_root / PLOT_NAME

    if args.replot_only:
        if not bc.run_has_matching_artifacts(output_root, run_config, [POINTS_CSV_NAME]):
            print(f"Error: no cached run found for replot under {output_root}", file=sys.stderr)
            return 2
        render_plot(read_points_csv(output_root / POINTS_CSV_NAME), plot_path)
        print(f"[tseitin] wrote plot {plot_path}")
        return 0

    if bc.run_has_matching_artifacts(output_root, run_config, [RAW_CSV_NAME, POINTS_CSV_NAME]) and not args.force_rerun:
        point_rows = read_points_csv(output_root / POINTS_CSV_NAME)
        print(f"[tseitin] reusing cached benchmark data from {output_root}")
    else:
        output_root.mkdir(parents=True, exist_ok=True)
        bc.write_run_config(output_root / "run_config.json", run_config)
        _, point_rows = execute_benchmark(args, generated_root=generated_root, run_dir=output_root, cache_root=cache_root)
        print(f"[tseitin] wrote raw data {output_root / RAW_CSV_NAME}")
        print(f"[tseitin] wrote points {output_root / POINTS_CSV_NAME}")

    render_plot(point_rows, plot_path)
    print(f"[tseitin] wrote plot {plot_path}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main(sys.argv[1:]))
    except (FileNotFoundError, RuntimeError, ValueError) as exc:
        print(f"Error: {exc}", file=sys.stderr)
        raise SystemExit(2)
