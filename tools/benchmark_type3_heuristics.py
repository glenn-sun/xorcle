#!/usr/bin/env python3
from __future__ import annotations

import argparse
import csv
import os
import random
import sys
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass
from pathlib import Path
from types import SimpleNamespace
from typing import Dict, List, Sequence, Tuple

os.environ.setdefault("MPLCONFIGDIR", str(Path("/tmp") / "parity-sat-cpp-mplconfig"))

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt

sys.path.append(str(Path(__file__).resolve().parent))
import run_benchmark as rb
from benchmark_plot_style import UNIFIED_DPI, UNIFIED_FONT_SIZE
from benchmark_orchestration import (
    PHYSICAL_FAMILIES,
    format_progress_line,
    load_physical_families,
    sample_grouped_instances,
    sample_key_for,
    should_report_progress,
)
from plot_cactus import SeriesData, draw_cactus_axes, series_points


def repo_root() -> Path:
    return Path(__file__).resolve().parents[1]


DECISION_PROCS = ("vsids_unit", "cvsids", "berkmin", "cmtf")
CLAUSE_SELECTORS = ("cyclic", "lt_watchlist", "exact_watchlist")
DECISION_PHASES = ("sat", "unsat", "random")
COMPACTION_MODES = (
    ("compact_on", ("-x", "on")),
    ("compact_off", ("-x", "off")),
)
ALL_TEST_IDS = ("decision_proc", "clause_selector", "decision_phase", "compaction")
TYPE3_PLOT_W = 2.0
TYPE3_PLOT_H = 1.5
TYPE3_LEGEND_GAP = 0.15
TYPE3_LEGEND_W = 1.35
TYPE3_OUTER_X = 0.4
TYPE3_OUTER_Y = 0.4
TYPE3_GUTTER_X = 0.5
TYPE3_GUTTER_Y = 0.75


@dataclass(frozen=True)
class VariantSpec:
    label: str
    display_name: str
    solver_passthrough: Tuple[str, ...]


@dataclass(frozen=True)
class TestSpec:
    test_id: str
    title: str
    subplot_title: str
    variants: Tuple[VariantSpec, ...]


TEST_SPECS: Dict[str, TestSpec] = {
    "decision_proc": TestSpec(
        test_id="decision_proc",
        title="Type 3: Decision Procedures",
        subplot_title="Decision Procedure",
        variants=(
            VariantSpec(label="vsids_unit", display_name="Variable VSIDS", solver_passthrough=("-d", "vsids_unit")),
            VariantSpec(label="cvsids", display_name="Clause VSIDS", solver_passthrough=("-d", "cvsids")),
            VariantSpec(label="berkmin", display_name="BerkMin-like", solver_passthrough=("-d", "berkmin")),
            VariantSpec(label="cmtf", display_name="CMTF-like", solver_passthrough=("-d", "cmtf")),
        ),
    ),
    "clause_selector": TestSpec(
        test_id="clause_selector",
        title="Type 3: Clause Selectors",
        subplot_title="Clause Selection",
        variants=(
            VariantSpec(label="cyclic", display_name="Cyclic", solver_passthrough=("-c", "cyclic")),
            VariantSpec(label="lt_watchlist", display_name="LT Watchlist", solver_passthrough=("-c", "lt_watchlist")),
            VariantSpec(label="exact_watchlist", display_name="Exact Watchlist", solver_passthrough=("-c", "exact_watchlist")),
        ),
    ),
    "decision_phase": TestSpec(
        test_id="decision_phase",
        title="Type 3: Decision Phases",
        subplot_title="Phase Selection",
        variants=(
            VariantSpec(label="sat", display_name="Satisfy", solver_passthrough=("-p", "sat")),
            VariantSpec(label="unsat", display_name="Falsify", solver_passthrough=("-p", "unsat")),
            VariantSpec(label="random", display_name="Random", solver_passthrough=("-p", "random")),
        ),
    ),
    "compaction": TestSpec(
        test_id="compaction",
        title="Type 3: Compaction (-x on vs off)",
        subplot_title="Compaction",
        variants=(
            VariantSpec(label="compact_on", display_name="On", solver_passthrough=("-x", "on")),
            VariantSpec(label="compact_off", display_name="Off", solver_passthrough=("-x", "off")),
        ),
    ),
}
COMBINED_TEST_ORDER = ("compaction", "clause_selector", "decision_proc", "decision_phase")


def parse_args() -> argparse.Namespace:
    root = repo_root()
    parser = argparse.ArgumentParser(description="Type 3 benchmark: heuristic sweeps for xorcle + cactus plots.")
    parser.add_argument("--tests-root", default=str(root / "tests"))
    parser.add_argument("--output-root", default=str(root / "benchmarks" / "type3"))
    parser.add_argument("--solver-bin", default=str(root / "bin" / "main-release"))
    parser.add_argument("--2xornado-bin", dest="xornado_bin", default=str(root / "external" / "2xornado"))
    parser.add_argument("--xorricane-bin", default=str(root / "external" / "xorricane"))
    parser.add_argument("--kissat-bin", default=str(root / "external" / "kissat"))
    parser.add_argument("--cms-bin", default=str(root / "external" / "cryptominisat5"))
    parser.add_argument("--timeout", type=float, default=5.0)
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("-k", "--samples-per-family", type=int, default=5)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--saveconverted", action="store_true")
    parser.add_argument("--only-tests", nargs="*", choices=ALL_TEST_IDS, default=list(ALL_TEST_IDS))
    parser.add_argument("--replot-only", action="store_true", help="Regenerate plots from existing CSV files only.")
    args = parser.parse_args()

    if args.timeout <= 0:
        parser.error("--timeout must be > 0")
    if args.jobs < 1:
        parser.error("--jobs must be >= 1")
    if args.samples_per_family < 1:
        parser.error("--samples-per-family must be >= 1")
    return args


def write_csv(path: Path, rows: Sequence[Dict[str, str]], fieldnames: Sequence[str]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fieldnames)
        w.writeheader()
        w.writerows(rows)


def make_runner_args(args: argparse.Namespace, input_format: str) -> SimpleNamespace:
    return SimpleNamespace(
        input_format=input_format,
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
        solvers=["xorcle_0_1"],
        saveconverted=bool(args.saveconverted),
    )


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


def run_variant(
    *,
    args: argparse.Namespace,
    sampled_records: Dict[str, List[object]],
    solver_passthrough: Sequence[str],
    converter_script: Path,
    on_file_done,
) -> List[Dict[str, str]]:
    tasks = []
    for family, records in sampled_records.items():
        for idx, record in enumerate(records, start=1):
            tasks.append((family, idx, record))

    def run_one(family: str, idx: int, record) -> List[Dict[str, str]]:
        rows = rb.benchmark_file(record.file, make_runner_args(args, record.input_format), solver_passthrough, converter_script)
        sample_key = sample_key_for(family=family, index=idx, file_path=record.file)
        for row in rows:
            row["sample_key"] = sample_key
        return rows

    rows: List[Dict[str, str]] = []
    with ThreadPoolExecutor(max_workers=args.jobs) as executor:
        futures = [executor.submit(run_one, family, idx, record) for family, idx, record in tasks]
        for future in as_completed(futures):
            rows.extend(future.result())
            on_file_done()

    rows.sort(
        key=lambda row: (
            row.get("sample_key", ""),
            int(row.get("run_index", "0") or 0),
            row.get("solver_config", ""),
            row.get("backend_input_kind", ""),
            row.get("solver", ""),
        )
    )
    return rows


def series_for_variants(variant_rows: Sequence[tuple[VariantSpec, Sequence[Dict[str, str]]]]) -> List[SeriesData]:
    return [SeriesData(label=variant.display_name, rows=list(rows)) for variant, rows in variant_rows]


def legend_label_for_series(series: SeriesData) -> str:
    _, solved, total = series_points(
        series.rows,
        aggregate_per_file="median",
        include_timeouts=False,
        metric="runtime",
    )
    return f"{series.label}\n({solved}/{total} solved)"


def type3_block_width() -> float:
    return TYPE3_PLOT_W + TYPE3_LEGEND_GAP + TYPE3_LEGEND_W


def type3_figure_size(*, rows: int, cols: int) -> tuple[float, float]:
    width = (2 * TYPE3_OUTER_X) + (cols * type3_block_width()) + ((cols - 1) * TYPE3_GUTTER_X)
    height = (2 * TYPE3_OUTER_Y) + (rows * TYPE3_PLOT_H) + ((rows - 1) * TYPE3_GUTTER_Y)
    return width, height


def type3_axes_rect(*, rows: int, cols: int, row: int, col: int) -> tuple[float, float, float, float]:
    fig_w, fig_h = type3_figure_size(rows=rows, cols=cols)
    left_in = TYPE3_OUTER_X + (col * (type3_block_width() + TYPE3_GUTTER_X))
    bottom_in = TYPE3_OUTER_Y + ((rows - 1 - row) * (TYPE3_PLOT_H + TYPE3_GUTTER_Y))
    return (
        left_in / fig_w,
        bottom_in / fig_h,
        TYPE3_PLOT_W / fig_w,
        TYPE3_PLOT_H / fig_h,
    )


def render_type3_panel(fig, rect, *, title: str, series: Sequence[SeriesData]) -> None:
    ax = fig.add_axes(rect)
    draw_cactus_axes(
        ax,
        series,
        aggregate_per_file="median",
        include_timeouts=False,
        metric="runtime",
        line_mode="line",
        black_white=False,
        title=title,
        xlabel="Instances solved",
        ylabel="Runtime (s)",
        x_scale="linear",
        y_scale="linear",
        x_ticks=[],
        y_ticks=[],
        font_size=UNIFIED_FONT_SIZE,
        show_counts=False,
    )
    handles = list(ax.get_lines())
    labels = [legend_label_for_series(s) for s in series]
    legend = ax.legend(
        handles,
        labels,
        fontsize=UNIFIED_FONT_SIZE,
        frameon=False,
        loc="center left",
        bbox_to_anchor=(1.0 + (TYPE3_LEGEND_GAP / TYPE3_PLOT_W), 0.5),
        borderaxespad=0.0,
        handleheight=1.4,
        labelspacing=0.8,
    )
    for text in legend.get_texts():
        text.set_multialignment("left")


def render_type3_single(
    *,
    output_path: Path,
    title: str,
    variant_rows: Sequence[tuple[VariantSpec, Sequence[Dict[str, str]]]],
) -> None:
    series = series_for_variants(variant_rows)
    if not series:
        raise RuntimeError(f"plot generation failed for {output_path.name}: no matching series")
    fig = plt.figure(figsize=type3_figure_size(rows=1, cols=1))
    render_type3_panel(
        fig,
        type3_axes_rect(rows=1, cols=1, row=0, col=0),
        title=title,
        series=series,
    )
    output_path.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(output_path.resolve(), dpi=UNIFIED_DPI)
    plt.close(fig)


def all_tests_selected(selected_tests: Sequence[str]) -> bool:
    return set(selected_tests) == set(ALL_TEST_IDS)


def combined_plot_path(plots_root: Path) -> Path:
    return plots_root / "all_heuristics.png"


def generate_combined_cactus_plot(
    *,
    output_path: Path,
    per_test_variant_rows: Dict[str, Sequence[tuple[VariantSpec, Sequence[Dict[str, str]]]]],
) -> None:
    fig = plt.figure(figsize=type3_figure_size(rows=2, cols=2))
    for index, test_id in enumerate(COMBINED_TEST_ORDER):
        row = index // 2
        col = index % 2
        render_type3_panel(
            fig,
            type3_axes_rect(rows=2, cols=2, row=row, col=col),
            title=TEST_SPECS[test_id].subplot_title,
            series=series_for_variants(per_test_variant_rows[test_id]),
        )
    output_path.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(output_path.resolve(), dpi=UNIFIED_DPI)
    plt.close(fig)


def replot_from_existing_csv(*, args: argparse.Namespace, output_root: Path) -> int:
    csv_root = output_root / "csv"
    plots_root = output_root / "plots"
    plots_root.mkdir(parents=True, exist_ok=True)
    combined_inputs: Dict[str, Sequence[tuple[VariantSpec, Sequence[Dict[str, str]]]]] = {}

    plotted = 0
    for test_id in args.only_tests:
        test_spec = TEST_SPECS[test_id]
        test_csv_dir = csv_root / test_id
        variant_rows: List[tuple[VariantSpec, Sequence[Dict[str, str]]]] = []
        for variant in test_spec.variants:
            csv_path = test_csv_dir / f"{variant.label}.raw.csv"
            if not csv_path.is_file():
                print(f"[type3] skipping {test_id}/{variant.label}: missing CSV {csv_path}")
                continue
            with csv_path.open("r", encoding="utf-8", newline="") as f:
                rows = [row for row in csv.DictReader(f) if (row.get("solver") or "").strip() == "xorcle_0_1"]
            variant_rows.append((variant, rows))

        if not variant_rows:
            print(f"[type3] skipping {test_id}: no CSV inputs found under {test_csv_dir}")
            continue

        plot_path = plots_root / f"{test_id}.png"
        render_type3_single(output_path=plot_path, title=test_spec.subplot_title, variant_rows=variant_rows)
        if len(variant_rows) == len(test_spec.variants):
            combined_inputs[test_id] = variant_rows
        plotted += 1
        print(f"[type3] replot wrote plot {plot_path}")

    if all_tests_selected(args.only_tests):
        missing = [test_id for test_id in COMBINED_TEST_ORDER if test_id not in combined_inputs]
        if missing:
            print(f"[type3] skipping combined plot: missing CSVs for {', '.join(missing)}")
        else:
            output_path = combined_plot_path(plots_root)
            generate_combined_cactus_plot(output_path=output_path, per_test_variant_rows=combined_inputs)
            print(f"[type3] replot wrote plot {output_path}")

    if plotted == 0:
        print(f"[type3] no plots regenerated from {csv_root}")
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
        return replot_from_existing_csv(args=args, output_root=output_root)

    all_families = load_physical_families(tests_root, PHYSICAL_FAMILIES)
    sampled = sample_grouped_instances(
        all_families,
        count_per_family=args.samples_per_family,
        rng=random.Random(args.seed),
        with_replacement=True,
    )

    manifest_path = csv_root / "selected_instances.csv"
    write_csv(
        manifest_path,
        sampled_manifest_rows(sampled),
        ["family", "sample_index", "sample_key", "file", "input_format", "attempt", "n", "k", "h", "lift_k"],
    )
    print(f"[type3] wrote sample manifest: {manifest_path}")

    total_tasks = sum(len(records) for records in sampled.values()) * sum(len(TEST_SPECS[test_id].variants) for test_id in args.only_tests)
    progress_every = max(1, total_tasks // 40)
    start_all = time.perf_counter()
    progress_state = {"done": 0}
    combined_inputs: Dict[str, Sequence[tuple[VariantSpec, Sequence[Dict[str, str]]]]] = {}

    def mark_task_done(test_id: str, variant_label: str) -> None:
        progress_state["done"] += 1
        done = progress_state["done"]
        if should_report_progress(done, total_tasks, progress_every):
            print(format_progress_line(done=done, total=total_tasks, start_s=start_all, test=test_id, variant=variant_label))

    for test_id in args.only_tests:
        test_spec = TEST_SPECS[test_id]
        test_csv_dir = csv_root / test_id
        test_csv_dir.mkdir(parents=True, exist_ok=True)

        plot_variant_rows: List[tuple[VariantSpec, Sequence[Dict[str, str]]]] = []
        print(f"[type3] running {test_id}: {test_spec.title}")
        for variant in test_spec.variants:
            rows = run_variant(
                args=args,
                sampled_records=sampled,
                solver_passthrough=list(variant.solver_passthrough),
                converter_script=converter_script,
                on_file_done=lambda tid=test_id, vlabel=variant.label: mark_task_done(tid, vlabel),
            )
            csv_path = test_csv_dir / f"{variant.label}.raw.csv"
            write_csv(csv_path, rows, rb.RAW_CSV_FIELDS)
            plot_variant_rows.append((variant, [row for row in rows if (row.get("solver") or "").strip() == "xorcle_0_1"]))
            print(f"[type3] {test_id}/{variant.label}: wrote {csv_path} ({len(rows)} rows)")

        plot_path = plots_root / f"{test_id}.png"
        render_type3_single(output_path=plot_path, title=test_spec.subplot_title, variant_rows=plot_variant_rows)
        print(f"[type3] {test_id}: wrote plot {plot_path}")
        if len(plot_variant_rows) == len(test_spec.variants):
            combined_inputs[test_id] = plot_variant_rows

    if all_tests_selected(args.only_tests):
        missing = [test_id for test_id in COMBINED_TEST_ORDER if test_id not in combined_inputs]
        if missing:
            print(f"[type3] skipping combined plot: missing data for {', '.join(missing)}")
        else:
            output_path = combined_plot_path(plots_root)
            generate_combined_cactus_plot(output_path=output_path, per_test_variant_rows=combined_inputs)
            print(f"[type3] wrote plot {output_path}")

    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (FileNotFoundError, RuntimeError, ValueError) as exc:
        print(f"Error: {exc}", file=sys.stderr)
        raise SystemExit(2)
