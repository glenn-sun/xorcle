#!/usr/bin/env python3
from __future__ import annotations

import argparse
import csv
import math
import os
import random
import sys
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path
from types import SimpleNamespace
from typing import Dict, List, Sequence

os.environ.setdefault("MPLCONFIGDIR", str(Path("/tmp") / "parity-sat-cpp-mplconfig"))

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import FixedLocator, LogFormatterMathtext, NullFormatter, NullLocator

sys.path.append(str(Path(__file__).resolve().parent))
import run_benchmark as bs
from benchmark_plot_style import UNIFIED_DPI, UNIFIED_FONT_SIZE, unified_figure_geometry
from benchmark_orchestration import (
    ALL_SOLVER_LINES,
    DEFAULT_SOLVER_LINES,
    PHYSICAL_FAMILIES,
    SOLVER_DISPLAY,
    family_slug,
    format_progress_line,
    load_physical_families,
    sample_grouped_instances,
    sample_key_for,
    should_report_progress,
    solver_line_key,
)
from plot_cactus import SeriesData, draw_cactus_axes, series_points


def repo_root() -> Path:
    return Path(__file__).resolve().parents[1]


def parse_args() -> argparse.Namespace:
    root = repo_root()
    parser = argparse.ArgumentParser(description="Type 1 benchmark: cactus plots for all benchmark families.")
    parser.add_argument("--tests-root", default=str(root / "tests"))
    parser.add_argument("--output-root", default=str(root / "benchmarks" / "type1"))
    parser.add_argument("--solver-bin", default=str(root / "bin" / "main-release"))
    parser.add_argument("--2xornado-bin", dest="xornado_bin", default=str(root / "external" / "2xornado"))
    parser.add_argument("--xorricane-bin", default=str(root / "external" / "xorricane"))
    parser.add_argument("--kissat-bin", default=str(root / "external" / "kissat"))
    parser.add_argument("--cms-bin", default=str(root / "external" / "cryptominisat5"))
    parser.add_argument("--attempts", type=int, default=1, help="Solver reruns on the same sampled instance")
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--timeout", type=float, default=5.0)
    parser.add_argument("--samples-per-family", type=int, default=20)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--all", action="store_true", help="Benchmark every instance in each selected family")
    parser.add_argument("--saveconverted", action="store_true")
    parser.add_argument(
        "--families",
        nargs="*",
        choices=list(PHYSICAL_FAMILIES),
        default=list(PHYSICAL_FAMILIES),
        help="Physical family subset to benchmark",
    )
    parser.add_argument(
        "--solvers",
        nargs="*",
        choices=list(ALL_SOLVER_LINES),
        default=None,
        help="Optional solver subset. Default excludes xorricane.",
    )
    parser.add_argument("--replot-only", action="store_true", help="Regenerate plots from existing CSV files only.")
    args = parser.parse_args()

    if args.attempts < 1:
        parser.error("--attempts must be >= 1")
    if args.jobs < 1:
        parser.error("--jobs must be >= 1")
    if args.timeout <= 0:
        parser.error("--timeout must be > 0")
    if args.samples_per_family < 1:
        parser.error("--samples-per-family must be >= 1")
    return args


def selected_solver_lines(args: argparse.Namespace) -> List[str]:
    return list(args.solvers) if args.solvers else list(DEFAULT_SOLVER_LINES)


def make_runner_args(args: argparse.Namespace, *, input_format: str, solvers: Sequence[str]) -> SimpleNamespace:
    return SimpleNamespace(
        input_format=input_format,
        repeat=args.attempts,
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
        solvers=list(solvers),
        saveconverted=bool(args.saveconverted),
    )


def write_csv(path: Path, rows: Sequence[Dict[str, str]], fieldnames: Sequence[str]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fieldnames)
        w.writeheader()
        w.writerows(rows)


def read_csv_rows(path: Path) -> List[Dict[str, str]]:
    with path.open("r", encoding="utf-8", newline="") as f:
        return list(csv.DictReader(f))


def raw_csv_path(csv_root: Path, family: str) -> Path:
    return csv_root / f"{family_slug(family)}.raw.csv"


def family_plot_path(plots_root: Path, family: str) -> Path:
    return plots_root / f"{family_slug(family)}.png"


def combined_plot_path(plots_root: Path) -> Path:
    return plots_root / "all_except_satcomp.png"


def satcomp_ascon_plot_path(plots_root: Path) -> Path:
    return plots_root / "satcomp_ascon.png"


def build_series(rows: Sequence[Dict[str, str]], solver_lines: Sequence[str]) -> List[SeriesData]:
    return [
        SeriesData(label=SOLVER_DISPLAY[solver], rows=[row for row in rows if solver_line_key(row) == solver])
        for solver in solver_lines
    ]


def legend_label_for_series(series: SeriesData) -> str:
    _, solved, total = series_points(
        series.rows,
        aggregate_per_file="median",
        include_timeouts=False,
        metric="runtime",
    )
    return f"{series.label}\n({solved}/{total} solved)"


def solved_total_for_series(series: SeriesData) -> tuple[int, int]:
    _, solved, total = series_points(
        series.rows,
        aggregate_per_file="median",
        include_timeouts=False,
        metric="runtime",
    )
    return solved, total


def satcomp_ascon_legend_labels(*, satcomp_series: Sequence[SeriesData], ascon_series: Sequence[SeriesData]) -> List[str]:
    satcomp_by_label = {series.label: solved_total_for_series(series) for series in satcomp_series}
    ascon_by_label = {series.label: solved_total_for_series(series) for series in ascon_series}
    labels: List[str] = []
    for series in satcomp_series:
        sc_solved, sc_total = satcomp_by_label[series.label]
        as_solved, as_total = ascon_by_label[series.label]
        labels.append(f"{series.label}\n({sc_solved}/{sc_total} SC, {as_solved}/{as_total} Ascon)")
    return labels


def configure_decisions_axis(ax) -> None:
    ymin, ymax = ax.get_ylim()
    if ymin <= 0.0 or ymax <= 0.0:
        return

    min_exp = math.floor(math.log10(ymin))
    max_exp = math.ceil(math.log10(ymax))
    major_start = min_exp if min_exp % 2 == 0 else min_exp + 1
    major_exponents = list(range(major_start, max_exp + 1, 2))
    major_ticks = [10.0 ** exp for exp in major_exponents if ymin <= 10.0 ** exp <= ymax]

    ax.yaxis.set_major_locator(FixedLocator(major_ticks))
    ax.yaxis.set_major_formatter(LogFormatterMathtext(base=10.0))
    ax.yaxis.set_minor_locator(NullLocator())
    ax.yaxis.set_minor_formatter(NullFormatter())
    ax.xaxis.set_minor_locator(NullLocator())
    ax.yaxis.set_ticks_position("left")
    ax.tick_params(
        axis="y",
        which="major",
        left=True,
        right=False,
        labelleft=True,
        labelright=False,
        direction="in",
        length=5.0,
        width=0.9,
        colors="black",
    )
    ax.tick_params(
        axis="y",
        which="minor",
        left=False,
        right=False,
        labelleft=False,
        labelright=False,
        length=0.0,
    )
    ax.tick_params(axis="x", which="minor", bottom=False, top=False)
    ax.set_axisbelow(True)
    ax.grid(True, which="major", axis="y", linestyle="--", linewidth=0.6, alpha=0.40, color="#666666")


def render_cactus_pair(*, title: str, rows: Sequence[Dict[str, str]], solver_lines: Sequence[str], output_path: Path) -> None:
    series = build_series(rows, solver_lines)
    base_width, base_height = unified_figure_geometry()[0]
    fig, axes = plt.subplots(1, 2, figsize=(base_width * 1.46, base_height * 0.98))

    draw_cactus_axes(
        axes[0],
        series,
        aggregate_per_file="median",
        include_timeouts=False,
        metric="runtime",
        line_mode="line",
        black_white=False,
        title="Runtime",
        xlabel="Instances solved",
        ylabel="",
        x_scale="linear",
        y_scale="linear",
        x_ticks=[],
        y_ticks=[],
        font_size=UNIFIED_FONT_SIZE,
        show_counts=True,
    )
    draw_cactus_axes(
        axes[1],
        series,
        aggregate_per_file="median",
        include_timeouts=False,
        metric="decisions",
        line_mode="line",
        black_white=False,
        title="Decisions (log scale)",
        xlabel="Instances solved",
        ylabel="",
        x_scale="linear",
        y_scale="log",
        x_ticks=[],
        y_ticks=[],
        font_size=UNIFIED_FONT_SIZE,
        show_counts=True,
    )
    configure_decisions_axis(axes[1])

    handles = list(axes[0].get_lines())
    labels = [legend_label_for_series(s) for s in series]
    for ax in axes:
        legend = ax.get_legend()
        if legend is not None:
            legend.remove()
    legend = fig.legend(
        handles,
        labels,
        fontsize=UNIFIED_FONT_SIZE,
        frameon=False,
        loc="center left",
        bbox_to_anchor=(0.782, 0.5),
        borderaxespad=0.0,
        handleheight=1.4,
        labelspacing=0.8,
    )
    for text in legend.get_texts():
        text.set_multialignment("left")
    fig.subplots_adjust(left=0.068, right=0.755, bottom=0.17, top=0.87, wspace=0.20)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(output_path, dpi=UNIFIED_DPI, bbox_inches="tight", pad_inches=0.04)
    plt.close(fig)


def render_family_plots(*, family: str, rows: Sequence[Dict[str, str]], solver_lines: Sequence[str], plots_root: Path) -> None:
    render_cactus_pair(
        title=f"Type 1 Cactus ({family})",
        rows=rows,
        solver_lines=solver_lines,
        output_path=family_plot_path(plots_root, family),
    )


def should_render_combined_plot(families: Sequence[str]) -> bool:
    return set(families) == set(PHYSICAL_FAMILIES)


def render_combined_plot(*, family_rows: Dict[str, Sequence[Dict[str, str]]], solver_lines: Sequence[str], plots_root: Path) -> None:
    combined_rows: List[Dict[str, str]] = []
    for family in PHYSICAL_FAMILIES:
        if family == "sat-comp":
            continue
        combined_rows.extend(family_rows.get(family, []))
    render_cactus_pair(
        title="Type 1 Cactus (All Except sat-comp)",
        rows=combined_rows,
        solver_lines=solver_lines,
        output_path=combined_plot_path(plots_root),
    )


def render_satcomp_ascon_plot(*, family_rows: Dict[str, Sequence[Dict[str, str]]], solver_lines: Sequence[str], plots_root: Path) -> None:
    satcomp_series = build_series(family_rows["sat-comp"], solver_lines)
    ascon_series = build_series(family_rows["ascon"], solver_lines)
    base_width, base_height = unified_figure_geometry()[0]
    fig, axes = plt.subplots(1, 2, figsize=(base_width * 1.46, base_height * 0.98))

    draw_cactus_axes(
        axes[0],
        satcomp_series,
        aggregate_per_file="median",
        include_timeouts=False,
        metric="runtime",
        line_mode="line",
        black_white=False,
        title="SAT Competition '24 and '25",
        xlabel="Instances solved",
        ylabel="Time (s)",
        x_scale="linear",
        y_scale="linear",
        x_ticks=[],
        y_ticks=[],
        font_size=UNIFIED_FONT_SIZE,
        show_counts=False,
    )
    draw_cactus_axes(
        axes[1],
        ascon_series,
        aggregate_per_file="median",
        include_timeouts=False,
        metric="runtime",
        line_mode="line",
        black_white=False,
        title="Ascon-128",
        xlabel="Instances solved",
        ylabel="Time(s)",
        x_scale="linear",
        y_scale="linear",
        x_ticks=[],
        y_ticks=[],
        font_size=UNIFIED_FONT_SIZE,
        show_counts=False,
    )

    handles = list(axes[0].get_lines())
    labels = satcomp_ascon_legend_labels(satcomp_series=satcomp_series, ascon_series=ascon_series)
    for ax in axes:
        legend = ax.get_legend()
        if legend is not None:
            legend.remove()
    legend = fig.legend(
        handles,
        labels,
        fontsize=UNIFIED_FONT_SIZE,
        frameon=False,
        loc="center left",
        bbox_to_anchor=(0.782, 0.5),
        borderaxespad=0.0,
        handleheight=1.4,
        labelspacing=0.8,
    )
    for text in legend.get_texts():
        text.set_multialignment("left")
    fig.subplots_adjust(left=0.085, right=0.755, bottom=0.17, top=0.87, wspace=0.24)
    output_path = satcomp_ascon_plot_path(plots_root)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(output_path, dpi=UNIFIED_DPI, bbox_inches="tight", pad_inches=0.04)
    plt.close(fig)


def replot_from_existing_csv(*, args: argparse.Namespace, output_root: Path) -> int:
    csv_root = output_root / "csv"
    plots_root = output_root / "plots"
    plots_root.mkdir(parents=True, exist_ok=True)
    solver_lines = selected_solver_lines(args)
    family_rows: Dict[str, List[Dict[str, str]]] = {}

    plotted = 0
    for family in args.families:
        csv_path = raw_csv_path(csv_root, family)
        if not csv_path.is_file():
            print(f"[type1] skipping {family}: missing CSV {csv_path}")
            continue
        rows = read_csv_rows(csv_path)
        family_rows[family] = rows
        render_family_plots(family=family, rows=rows, solver_lines=solver_lines, plots_root=plots_root)
        plotted += 1
        print(f"[type1] replot wrote: {family_plot_path(plots_root, family)}")

    if should_render_combined_plot(args.families):
        missing = [family for family in PHYSICAL_FAMILIES if family != "sat-comp" and family not in family_rows]
        if missing:
            print(f"[type1] skipping combined plot: missing CSVs for {', '.join(missing)}")
        else:
            render_combined_plot(family_rows=family_rows, solver_lines=solver_lines, plots_root=plots_root)
            print(f"[type1] replot wrote: {combined_plot_path(plots_root)}")
        pair_missing = [family for family in ("sat-comp", "ascon") if family not in family_rows]
        if pair_missing:
            print(f"[type1] skipping satcomp+ascon plot: missing CSVs for {', '.join(pair_missing)}")
        else:
            render_satcomp_ascon_plot(family_rows=family_rows, solver_lines=solver_lines, plots_root=plots_root)
            print(f"[type1] replot wrote: {satcomp_ascon_plot_path(plots_root)}")

    if plotted == 0:
        print(f"[type1] no plots regenerated from {csv_root}")
    return 0


def sampled_manifest_rows(grouped_records: Dict[str, List[object]]) -> List[Dict[str, str]]:
    rows: List[Dict[str, str]] = []
    for family, records in grouped_records.items():
        for idx, record in enumerate(records, start=1):
            rows.append(
                {
                    "family": family,
                    "sample_index": str(idx),
                    "sample_key": sample_key_for(family=family, index=idx, file_path=record.file),
                    "file": str(record.file),
                    "input_format": record.input_format,
                    "attempt": str(record.attempt),
                    "n": "" if record.n is None else str(record.n),
                    "k": "" if record.k is None else str(record.k),
                    "h": "" if record.h is None else str(record.h),
                    "lift_k": "" if record.lift_k is None else str(record.lift_k),
                }
            )
    return rows


def main() -> int:
    args = parse_args()
    root = repo_root()
    tests_root = Path(args.tests_root).resolve()
    output_root = Path(args.output_root).resolve()
    csv_root = output_root / "csv"
    plots_root = output_root / "plots"
    converter_script = root / "tools" / "xnf_convert.py"
    solver_lines = selected_solver_lines(args)

    csv_root.mkdir(parents=True, exist_ok=True)
    plots_root.mkdir(parents=True, exist_ok=True)

    if args.replot_only:
        return replot_from_existing_csv(args=args, output_root=output_root)

    family_records = load_physical_families(tests_root, args.families)
    rng = random.Random(args.seed)
    if args.all:
        sampled_records = family_records
    else:
        sampled_records = sample_grouped_instances(
            family_records,
            count_per_family=args.samples_per_family,
            rng=rng,
            with_replacement=True,
        )

    manifest_rows = sampled_manifest_rows(sampled_records)
    manifest_path = csv_root / "selected_instances.csv"
    write_csv(
        manifest_path,
        manifest_rows,
        ["family", "sample_index", "sample_key", "file", "input_format", "attempt", "n", "k", "h", "lift_k"],
    )
    print(f"[type1] wrote sample manifest: {manifest_path}")

    total_tasks = sum(len(records) for records in sampled_records.values())
    progress_every = max(1, total_tasks // 40)
    done = 0
    start_all = time.perf_counter()
    family_rows_by_family: Dict[str, List[Dict[str, str]]] = {}

    def run_sample(record, sample_key: str) -> List[Dict[str, str]]:
        runner_args = make_runner_args(args, input_format=record.input_format, solvers=solver_lines)
        rows = bs.benchmark_file(record.file, runner_args, [], converter_script)
        for row in rows:
            row["sample_key"] = sample_key
        return rows

    for family in args.families:
        records = sampled_records[family]
        family_rows: List[Dict[str, str]] = []
        with ThreadPoolExecutor(max_workers=args.jobs) as executor:
            futures = {
                executor.submit(
                    run_sample,
                    record,
                    sample_key_for(family=family, index=idx, file_path=record.file),
                ): idx
                for idx, record in enumerate(records, start=1)
            }
            for future in as_completed(futures):
                family_rows.extend(future.result())
                done += 1
                if should_report_progress(done, total_tasks, progress_every):
                    print(format_progress_line(done=done, total=total_tasks, start_s=start_all, family=family))

        family_rows.sort(
            key=lambda row: (
                row.get("sample_key", ""),
                int(row.get("run_index", "0") or 0),
                row.get("solver_config", ""),
                row.get("backend_input_kind", ""),
                row.get("solver", ""),
            )
        )

        csv_path = raw_csv_path(csv_root, family)
        write_csv(csv_path, family_rows, bs.RAW_CSV_FIELDS)
        family_rows_by_family[family] = family_rows
        render_family_plots(family=family, rows=family_rows, solver_lines=solver_lines, plots_root=plots_root)
        print(f"[type1] wrote {csv_path}")
        print(f"[type1] wrote {family_plot_path(plots_root, family)}")

    if should_render_combined_plot(args.families):
        render_combined_plot(family_rows=family_rows_by_family, solver_lines=solver_lines, plots_root=plots_root)
        print(f"[type1] wrote {combined_plot_path(plots_root)}")
        render_satcomp_ascon_plot(family_rows=family_rows_by_family, solver_lines=solver_lines, plots_root=plots_root)
        print(f"[type1] wrote {satcomp_ascon_plot_path(plots_root)}")

    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (FileNotFoundError, RuntimeError, ValueError) as exc:
        print(f"Error: {exc}", file=sys.stderr)
        raise SystemExit(2)
