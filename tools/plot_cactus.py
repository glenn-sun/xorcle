#!/usr/bin/env python3
"""Shared cactus-plot helpers plus a generic raw-CSV plotting CLI."""

from __future__ import annotations

import argparse
import csv
import os
import statistics
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Tuple

# Matplotlib needs a writable config/cache directory in this environment.
os.environ.setdefault("MPLCONFIGDIR", str(Path("/tmp") / "parity-sat-cpp-mplconfig"))
from benchmark_plot_style import (
    UNIFIED_DPI,
    UNIFIED_FONT_SIZE,
    apply_common_axes_style,
    create_unified_figure,
    finalize_figure,
    place_right_legend,
    series_style,
    unified_figure_geometry,
)
from benchmark_orchestration import aggregation_key


def import_matplotlib():
    try:
        import matplotlib

        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        from matplotlib.ticker import FixedLocator, FuncFormatter
    except ModuleNotFoundError as exc:  # pragma: no cover
        raise SystemExit(f"Error: missing dependency '{exc.name}'. Install matplotlib to run plotting.")
    return plt, FixedLocator, FuncFormatter


@dataclass(frozen=True)
class InputSpec:
    path: Path
    label: str


@dataclass(frozen=True)
class FilterSpec:
    target_label: Optional[str]
    column: str
    value: str


@dataclass
class SeriesData:
    label: str
    rows: List[Dict[str, str]]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Generate cactus plots from run_benchmark.py raw CSV files."
    )
    parser.add_argument(
        "--input",
        action="append",
        required=True,
        help="Input spec: PATH or PATH::LABEL. Repeat for multiple series/sources.",
    )
    parser.add_argument(
        "--where",
        action="append",
        default=[],
        help="Filter expression: COL=VALUE (global) or LABEL:COL=VALUE (targeted input label). Repeatable.",
    )
    parser.add_argument(
        "--group-by",
        default="",
        help="Optional row column to split series by (e.g. solver_config).",
    )
    parser.add_argument(
        "--prefix-source",
        action="store_true",
        help="When --group-by is set, prefix series label with source label.",
    )
    parser.add_argument(
        "--aggregate-per-file",
        choices=["none", "median", "mean", "min"],
        default="median",
        help="How to combine repeated runs of the same file in a series (default: median).",
    )
    parser.add_argument(
        "--include-timeouts",
        action="store_true",
        help="Include TIMEOUT and MEMORY rows as capped points in cactus data.",
    )
    parser.add_argument(
        "--metric",
        choices=["runtime", "decisions"],
        default="runtime",
        help="Metric to plot (default: runtime).",
    )
    parser.add_argument(
        "--line-mode",
        choices=["line", "step"],
        default="line",
        help="line: connect sorted points with straight segments; step: staircase curve.",
    )
    parser.add_argument(
        "--black-white",
        dest="black_white",
        action="store_true",
        default=False,
        help="Render all series in black while keeping distinct line styles.",
    )
    parser.add_argument(
        "--color",
        dest="black_white",
        action="store_false",
        help="Use color plus distinct line styles (default).",
    )
    parser.add_argument(
        "--title",
        default="Cactus Plot",
        help="Plot title.",
    )
    parser.add_argument(
        "--xlabel",
        default="Instances solved",
        help="X-axis label.",
    )
    parser.add_argument(
        "--ylabel",
        default="Runtime (s)",
        help="Y-axis label.",
    )
    parser.add_argument(
        "--x-scale",
        choices=["linear", "log"],
        default="linear",
        help="X-axis scale (default: linear).",
    )
    parser.add_argument(
        "--y-scale",
        choices=["linear", "log"],
        default="log",
        help="Y-axis scale (default: log).",
    )
    parser.add_argument(
        "--x-ticks",
        nargs="*",
        type=float,
        default=[],
        help="Optional explicit major x ticks.",
    )
    parser.add_argument(
        "--y-ticks",
        nargs="*",
        type=float,
        default=[],
        help="Optional explicit major y ticks.",
    )
    parser.add_argument(
        "--font-size",
        type=float,
        default=UNIFIED_FONT_SIZE,
        help="Uniform font size for title/axes/ticks/legend.",
    )
    parser.add_argument(
        "--figsize",
        nargs=2,
        type=float,
        default=list(unified_figure_geometry()[0]),
        metavar=("W", "H"),
        help="Figure size in inches.",
    )
    parser.add_argument(
        "--dpi",
        type=int,
        default=UNIFIED_DPI,
        help="Output DPI.",
    )
    parser.add_argument(
        "--output",
        required=True,
        help="Output PNG path.",
    )
    parser.add_argument(
        "--show-counts",
        action="store_true",
        default=True,
        help="Append '(solved/total)' to legend labels (default: true).",
    )
    parser.add_argument(
        "--hide-counts",
        action="store_true",
        help="Disable count suffix in legend labels.",
    )

    args = parser.parse_args()
    if args.hide_counts:
        args.show_counts = False
    return args


def parse_input_spec(raw: str) -> InputSpec:
    if "::" in raw:
        path_part, label = raw.split("::", 1)
        label = label.strip()
        if not label:
            raise ValueError(f"invalid --input (empty label): {raw}")
    else:
        path_part = raw
        label = Path(raw).stem
    path = Path(path_part).expanduser().resolve()
    if not path.is_file():
        raise FileNotFoundError(f"input CSV not found: {path}")
    return InputSpec(path=path, label=label)


def parse_filter(raw: str) -> FilterSpec:
    if "=" not in raw:
        raise ValueError(f"invalid --where (expected COL=VALUE or LABEL:COL=VALUE): {raw}")
    lhs, value = raw.split("=", 1)
    if ":" in lhs:
        target, col = lhs.split(":", 1)
        target = target.strip()
        col = col.strip()
        if not target or not col:
            raise ValueError(f"invalid --where target/column: {raw}")
        return FilterSpec(target_label=target, column=col, value=value)
    col = lhs.strip()
    if not col:
        raise ValueError(f"invalid --where column: {raw}")
    return FilterSpec(target_label=None, column=col, value=value)


def read_csv_rows(path: Path) -> List[Dict[str, str]]:
    with path.open("r", encoding="utf-8", newline="") as f:
        return list(csv.DictReader(f))


def row_matches_filters(row: Dict[str, str], source_label: str, filters: Sequence[FilterSpec]) -> bool:
    for fs in filters:
        if fs.target_label is not None and fs.target_label != source_label:
            continue
        if row.get(fs.column, "") != fs.value:
            return False
    return True


def resolved_runtime_s(row: Dict[str, str], include_timeouts: bool) -> Optional[float]:
    status = (row.get("status") or "").strip().upper()
    if status in {"SAT", "UNSAT"}:
        raw = (row.get("parsed_solver_time_s") or "").strip()
        if not raw:
            raw = (row.get("elapsed_wall_s") or "").strip()
    elif include_timeouts and status in {"TIMEOUT", "MEMORY"}:
        raw = (row.get("timeout_s") or "").strip()
        if not raw:
            raw = (row.get("elapsed_wall_s") or "").strip()
    else:
        return None
    try:
        v = float(raw)
    except (TypeError, ValueError):
        return None
    return v if v > 0.0 else None


def resolved_decisions(row: Dict[str, str]) -> Optional[float]:
    status = (row.get("status") or "").strip().upper()
    if status not in {"SAT", "UNSAT"}:
        return None
    raw = (row.get("parsed_solver_decisions") or "").strip()
    try:
        v = float(raw)
    except (TypeError, ValueError):
        return None
    if v < 0.0:
        return None
    return v


def row_is_solved(row: Dict[str, str]) -> bool:
    return (row.get("status") or "").strip().upper() in {"SAT", "UNSAT"}


def reduce_values(values: Sequence[float], mode: str) -> float:
    if mode == "median":
        return float(statistics.median(values))
    if mode == "mean":
        return float(statistics.mean(values))
    if mode == "min":
        return float(min(values))
    raise ValueError(f"unknown reduce mode: {mode}")


def resolved_metric_value(row: Dict[str, str], metric: str, include_timeouts: bool) -> Optional[float]:
    if metric == "runtime":
        return resolved_runtime_s(row, include_timeouts)
    if metric == "decisions":
        return resolved_decisions(row)
    raise ValueError(f"unknown metric: {metric}")


def series_points(
    rows: Sequence[Dict[str, str]],
    aggregate_per_file: str,
    include_timeouts: bool,
    metric: str,
) -> Tuple[List[Tuple[int, float]], int, int]:
    if aggregate_per_file == "none":
        vals = sorted(
            t
            for row in rows
            if (t := resolved_metric_value(row, metric, include_timeouts)) is not None
        )
        solved = sum(1 for row in rows if row_is_solved(row))
        points = [(idx, value) for idx, value in enumerate(vals, start=1) if value > 0.0]
        return points, solved, len(rows)

    by_file: Dict[str, List[float]] = {}
    solved_total = 0
    file_total = 0
    seen_solved = set()
    seen_files = set()
    for row in rows:
        file_key = aggregation_key(row)
        if file_key not in seen_files:
            seen_files.add(file_key)
            file_total += 1
        if row_is_solved(row) and file_key not in seen_solved:
            seen_solved.add(file_key)
            solved_total += 1
        t = resolved_metric_value(row, metric, include_timeouts)
        if t is None:
            continue
        by_file.setdefault(file_key, []).append(t)

    vals = sorted(reduce_values(vs, aggregate_per_file) for vs in by_file.values() if vs)
    points = [(idx, value) for idx, value in enumerate(vals, start=1) if value > 0.0]
    return points, solved_total, file_total


def build_series(
    inputs: Sequence[InputSpec],
    filters: Sequence[FilterSpec],
    group_by: str,
    prefix_source: bool,
) -> List[SeriesData]:
    if not group_by:
        out: List[SeriesData] = []
        for spec in inputs:
            rows = [r for r in read_csv_rows(spec.path) if row_matches_filters(r, spec.label, filters)]
            out.append(SeriesData(label=spec.label, rows=rows))
        return out

    grouped: Dict[str, List[Dict[str, str]]] = {}
    for spec in inputs:
        for row in read_csv_rows(spec.path):
            if not row_matches_filters(row, spec.label, filters):
                continue
            key = row.get(group_by, "").strip()
            if not key:
                continue
            label = f"{spec.label}:{key}" if prefix_source else key
            grouped.setdefault(label, []).append(row)
    return [SeriesData(label=k, rows=v) for k, v in sorted(grouped.items(), key=lambda kv: kv[0])]


def plain_tick_label(v: float, _pos: int) -> str:
    if v <= 0:
        return ""
    if v < 1:
        s = f"{v:.6f}"
    else:
        s = f"{v:.2f}"
    s = s.rstrip("0").rstrip(".")
    return s if s else "0"


def draw_cactus_axes(
    ax,
    series: Sequence[SeriesData],
    *,
    aggregate_per_file: str,
    include_timeouts: bool,
    metric: str,
    line_mode: str,
    black_white: bool,
    title: str,
    xlabel: str,
    ylabel: str,
    x_scale: str,
    y_scale: str,
    x_ticks: Sequence[float],
    y_ticks: Sequence[float],
    font_size: float,
    show_counts: bool,
) -> None:
    plt, FixedLocator, FuncFormatter = import_matplotlib()
    color_cycle = plt.rcParams["axes.prop_cycle"].by_key().get("color", ["black"])
    for idx, s in enumerate(series):
        points, solved, total = series_points(s.rows, aggregate_per_file, include_timeouts, metric)
        xs = [point[0] for point in points]
        values = [point[1] for point in points]
        suffix = f" ({solved}/{total})" if show_counts else ""
        label = f"{s.label}{suffix}"

        style = series_style(idx)
        if black_white:
            color = "black"
        else:
            color = color_cycle[idx % len(color_cycle)] if color_cycle else style.color
        ls = style.linestyle
        linewidth = 1.3

        if values:
            if line_mode == "step":
                ax.step(xs, values, where="post", label=label, color=color, linestyle=ls, linewidth=linewidth)
            else:
                ax.plot(xs, values, label=label, color=color, linestyle=ls, linewidth=linewidth)
        else:
            ax.plot([], [], label=label, color=color, linestyle=ls, linewidth=linewidth)

    ax.set_xscale(x_scale)
    ax.set_yscale(y_scale)

    if x_ticks:
        ax.xaxis.set_major_locator(FixedLocator(list(x_ticks)))
    if y_ticks:
        ax.yaxis.set_major_locator(FixedLocator(list(y_ticks)))

    ax.xaxis.set_major_formatter(FuncFormatter(plain_tick_label))
    ax.yaxis.set_major_formatter(FuncFormatter(plain_tick_label))

    if x_scale == "log":
        ax.set_xticks([], minor=True)

    apply_common_axes_style(ax, xlabel=xlabel, ylabel=ylabel, title=title, font_size=font_size)


def plot_cactus(
    series: Sequence[SeriesData],
    output: Path,
    *,
    aggregate_per_file: str,
    include_timeouts: bool,
    metric: str,
    line_mode: str,
    black_white: bool,
    title: str,
    xlabel: str,
    ylabel: str,
    x_scale: str,
    y_scale: str,
    x_ticks: Sequence[float],
    y_ticks: Sequence[float],
    font_size: float,
    figsize: Sequence[float],
    dpi: int,
    show_counts: bool,
) -> None:
    plt, _, _ = import_matplotlib()
    fig, ax = create_unified_figure(plt)
    draw_cactus_axes(
        ax,
        series,
        aggregate_per_file=aggregate_per_file,
        include_timeouts=include_timeouts,
        metric=metric,
        line_mode=line_mode,
        black_white=black_white,
        title=title,
        xlabel=xlabel,
        ylabel=ylabel,
        x_scale=x_scale,
        y_scale=y_scale,
        x_ticks=x_ticks,
        y_ticks=y_ticks,
        font_size=font_size,
        show_counts=show_counts,
    )
    place_right_legend(ax, font_size=font_size)

    output.parent.mkdir(parents=True, exist_ok=True)
    finalize_figure(fig, output, dpi=dpi)
    plt.close(fig)


def main() -> int:
    args = parse_args()
    try:
        inputs = [parse_input_spec(v) for v in args.input]
        filters = [parse_filter(v) for v in args.where]
    except (ValueError, FileNotFoundError) as exc:
        print(f"Error: {exc}")
        return 2

    series = build_series(inputs, filters, args.group_by.strip(), args.prefix_source)
    if not series:
        print("Error: no series to plot after applying filters/grouping")
        return 2

    output = Path(args.output).expanduser().resolve()
    plot_cactus(
        series,
        output,
        aggregate_per_file=args.aggregate_per_file,
        include_timeouts=args.include_timeouts,
        metric=args.metric,
        line_mode=args.line_mode,
        black_white=args.black_white,
        title=args.title,
        xlabel=args.xlabel,
        ylabel=args.ylabel,
        x_scale=args.x_scale,
        y_scale=args.y_scale,
        x_ticks=args.x_ticks,
        y_ticks=args.y_ticks,
        font_size=args.font_size,
        figsize=args.figsize,
        dpi=args.dpi,
        show_counts=args.show_counts,
    )
    print(f"Wrote cactus plot -> {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
