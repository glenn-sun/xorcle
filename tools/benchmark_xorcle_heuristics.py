#!/usr/bin/env python3
from __future__ import annotations

import argparse
import csv
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from types import SimpleNamespace
from typing import Dict, List, Sequence, Tuple

import benchmark_core as bc
from benchmark_orchestration import REPRESENTATIVE_FAMILIES
from benchmark_orchestration import format_progress_line
from benchmark_orchestration import load_representative_sample
from benchmark_orchestration import manifest_rows_for_grouped_instances
from benchmark_orchestration import sample_key_for
from benchmark_orchestration import should_report_progress
from benchmark_orchestration import solver_line_key
from benchmark_plot_style import UNIFIED_DPI, UNIFIED_FONT_SIZE
from benchmark_plot_style import block_layout_axes_rect
from benchmark_plot_style import block_layout_figure_size
from benchmark_plot_style import block_layout_legend_anchor
from benchmark_plot_style import paper_legend_style
from benchmark_plot_style import xorcle_heuristics_layout
from plot_cactus import SeriesData, draw_cactus_axes, series_points
from tool_common import repo_root

BENCHMARK_KIND = "xorcle_heuristics"
MANIFEST_NAME = "selected_instances.csv"
PLOT_NAME = "xorcle_heuristics.png"
TEST_ORDER = ("compaction", "clause_selector", "decision_proc", "decision_phase")


def import_matplotlib_pyplot():
    try:
        import matplotlib

        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ModuleNotFoundError as exc:  # pragma: no cover
        raise SystemExit(f"Error: missing dependency '{exc.name}'. Install matplotlib to run plotting.")
    return plt


@dataclass(frozen=True)
class VariantSpec:
    label: str
    display_name: str
    solver_passthrough: Tuple[str, ...]


@dataclass(frozen=True)
class TestSpec:
    test_id: str
    subplot_title: str
    variants: Tuple[VariantSpec, ...]


TEST_SPECS: Dict[str, TestSpec] = {
    "compaction": TestSpec(
        test_id="compaction",
        subplot_title="Compaction",
        variants=(
            VariantSpec(label="compact_on", display_name="On", solver_passthrough=("-x", "on")),
            VariantSpec(label="compact_off", display_name="Off", solver_passthrough=("-x", "off")),
        ),
    ),
    "clause_selector": TestSpec(
        test_id="clause_selector",
        subplot_title="Clause Selection",
        variants=(
            VariantSpec(label="cyclic", display_name="Cyclic", solver_passthrough=("-c", "cyclic")),
            VariantSpec(label="lt_watchlist", display_name="LT Watchlist", solver_passthrough=("-c", "lt_watchlist")),
            VariantSpec(label="exact_watchlist", display_name="Exact Watchlist", solver_passthrough=("-c", "exact_watchlist")),
        ),
    ),
    "decision_proc": TestSpec(
        test_id="decision_proc",
        subplot_title="Decision Procedure",
        variants=(
            VariantSpec(label="clause_vsids", display_name="Clause VSIDS", solver_passthrough=("-d", "clause_vsids")),
            VariantSpec(label="variable_vsids", display_name="Variable VSIDS", solver_passthrough=("-d", "variable_vsids")),
            VariantSpec(label="berkmin", display_name="BerkMin-like", solver_passthrough=("-d", "berkmin")),
            VariantSpec(label="cmtf", display_name="CMTF-like", solver_passthrough=("-d", "cmtf")),
        ),
    ),
    "decision_phase": TestSpec(
        test_id="decision_phase",
        subplot_title="Phase Selection",
        variants=(
            VariantSpec(label="unsat", display_name="Falsify", solver_passthrough=("-p", "unsat")),
            VariantSpec(label="sat", display_name="Satisfy", solver_passthrough=("-p", "sat")),
            VariantSpec(label="random", display_name="Random", solver_passthrough=("-p", "random")),
        ),
    ),
}


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    root = repo_root()
    parser = argparse.ArgumentParser(description="Representative-set heuristic cactus plots for Xorcle.")
    parser.add_argument("--tests-root", default=str(root / "tests"))
    parser.add_argument("--output-root", default=str(root / "benchmarks" / "xorcle_heuristics"))
    parser.add_argument("--cache-root", default=str(root / "benchmarks" / "cache"))
    parser.add_argument("--timeout", type=float, default=5.0)
    parser.add_argument("--memory-limit-gb", type=float, default=bc.DEFAULT_BENCHMARK_MEMORY_LIMIT_GB)
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--representative-count", type=int, default=40, help="Instances to sample from each representative family.")
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--saveconverted", action="store_true")
    parser.add_argument("--force-rerun", action="store_true")
    parser.add_argument("--replot-only", action="store_true")
    parser.add_argument("--solver-bin", default=str(root / "bin" / "xorcle"))
    args = parser.parse_args(argv)

    if args.timeout <= 0:
        parser.error("--timeout must be > 0")
    if args.memory_limit_gb <= 0:
        parser.error("--memory-limit-gb must be > 0")
    if args.jobs < 1:
        parser.error("--jobs must be >= 1")
    if args.representative_count < 1:
        parser.error("--representative-count must be >= 1")
    return args


def make_runner_args(args: argparse.Namespace, input_format: str) -> SimpleNamespace:
    return SimpleNamespace(
        input_format=input_format,
        timeout=args.timeout,
        memory_limit_gb=args.memory_limit_gb,
        solver_bin=str(Path(args.solver_bin).resolve()),
        csv_path="",
        exp_cnf=False,
        solvers=["xorcle"],
        saveconverted=bool(args.saveconverted),
    )


def run_config_for_args(args: argparse.Namespace, tests_root: Path) -> Dict[str, object]:
    return {
        "benchmark_kind": BENCHMARK_KIND,
        "tests_root": str(tests_root),
        "families": list(REPRESENTATIVE_FAMILIES),
        "representative_count": int(args.representative_count),
        "seed": int(args.seed),
        "timeout": float(args.timeout),
        "memory_limit_gb": float(args.memory_limit_gb),
        "solver_bin": str(Path(args.solver_bin).resolve()),
        "saveconverted": bool(args.saveconverted),
        "tests": {
            test_id: [variant.label for variant in TEST_SPECS[test_id].variants]
            for test_id in TEST_ORDER
        },
    }


def write_manifest(path: Path, grouped_records) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as csv_file:
        writer = csv.DictWriter(
            csv_file,
            fieldnames=["family", "sample_index", "sample_key", "file", "input_format", "attempt", "n", "k", "h", "lift_k"],
        )
        writer.writeheader()
        writer.writerows(manifest_rows_for_grouped_instances(grouped_records))


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


def render_panel(fig, layout, *, row: int, col: int, title: str, series: Sequence[SeriesData]) -> None:
    legend_style = paper_legend_style()
    ax = fig.add_axes(block_layout_axes_rect(layout, row=row, col=col))
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
        ylabel="Time (s)",
        x_scale="linear",
        y_scale="linear",
        x_ticks=[],
        y_ticks=[],
        font_size=UNIFIED_FONT_SIZE,
        show_counts=False,
    )
    ax.legend(
        list(ax.get_lines()),
        [legend_label_for_series(item) for item in series],
        fontsize=UNIFIED_FONT_SIZE,
        frameon=False,
        loc="center left",
        bbox_to_anchor=block_layout_legend_anchor(layout, row=row, col=col),
        bbox_transform=fig.transFigure,
        borderaxespad=legend_style.borderaxespad,
        handleheight=legend_style.handleheight,
        labelspacing=legend_style.labelspacing,
    )


def render_plot(per_test_variant_rows: Dict[str, Sequence[tuple[VariantSpec, Sequence[Dict[str, str]]]]], output_path: Path) -> None:
    plt = import_matplotlib_pyplot()
    layout = xorcle_heuristics_layout()
    fig = plt.figure(figsize=block_layout_figure_size(layout))
    for index, test_id in enumerate(TEST_ORDER):
        render_panel(
            fig,
            layout,
            row=index // 2,
            col=index % 2,
            title=TEST_SPECS[test_id].subplot_title,
            series=series_for_variants(per_test_variant_rows[test_id]),
        )
    output_path.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(output_path, dpi=UNIFIED_DPI)
    plt.close(fig)


def execute_benchmark(args: argparse.Namespace, *, tests_root: Path, run_dir: Path, cache_root: Path) -> Dict[str, Sequence[tuple[VariantSpec, Sequence[Dict[str, str]]]]]:
    converter_script = repo_root() / "tools" / "xnf_convert.py"
    sampled_records = load_representative_sample(
        tests_root,
        count_per_family=args.representative_count,
        seed=args.seed,
    )
    write_manifest(run_dir / MANIFEST_NAME, sampled_records)
    plot_inputs: Dict[str, Sequence[tuple[VariantSpec, Sequence[Dict[str, str]]]]] = {}
    sample_keys = {
        str(record.file.resolve()): sample_key_for(family=family, index=index, file_path=record.file)
        for family, records in sampled_records.items()
        for index, record in enumerate(records, start=1)
    }
    files_by_format: Dict[str, List[Path]] = {}
    for records in sampled_records.values():
        for record in records:
            files_by_format.setdefault(record.input_format, []).append(record.file)
    unique_passthroughs = {
        tuple(variant.solver_passthrough)
        for test_id in TEST_ORDER
        for variant in TEST_SPECS[test_id].variants
    }
    total_tasks = len(sample_keys) * len(unique_passthroughs)
    progress_every = max(1, total_tasks // 40)
    progress_done = 0
    start_s = time.perf_counter()

    rows_by_variant_label: Dict[str, List[Dict[str, str]]] = {}
    rows_by_passthrough: Dict[Tuple[str, ...], List[Dict[str, str]]] = {}
    for test_id in TEST_ORDER:
        for variant in TEST_SPECS[test_id].variants:
            passthrough = tuple(variant.solver_passthrough)
            if passthrough in rows_by_passthrough:
                rows_by_variant_label[variant.label] = rows_by_passthrough[passthrough]
                continue
            rows: List[Dict[str, str]] = []
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
                            test=test_id,
                            variant=variant.label,
                            file=file_path.name,
                            reused="yes" if reused else "no",
                        )
                    )
            for input_format, selected_files in files_by_format.items():
                rows.extend(
                    bc.ensure_benchmark_file_coverage(
                        selected_files,
                        make_runner_args(args, input_format),
                        list(passthrough),
                        converter_script,
                        jobs=args.jobs,
                        cache_root=cache_root,
                        force_rerun=bool(args.force_rerun),
                        progress_callback=report_progress,
                    )
                )
            for row in rows:
                row["sample_key"] = sample_keys[str(Path(row["file"]).resolve())]
            rows = [
                row for row in sorted(
                    rows,
                    key=lambda row: (
                        row.get("sample_key", ""),
                        row.get("solver_config", ""),
                        row.get("backend_input_kind", ""),
                        row.get("solver", ""),
                    ),
                )
                if (row.get("solver") or "").strip() == "xorcle"
            ]
            rows_by_passthrough[passthrough] = rows
            rows_by_variant_label[variant.label] = rows

    for test_id in TEST_ORDER:
        test_spec = TEST_SPECS[test_id]
        test_dir = run_dir / test_id
        test_dir.mkdir(parents=True, exist_ok=True)
        variant_rows: List[tuple[VariantSpec, Sequence[Dict[str, str]]]] = []
        for variant in test_spec.variants:
            rows = rows_by_variant_label[variant.label]
            bc.write_raw_csv(test_dir / f"{variant.label}.raw.csv", rows)
            variant_rows.append((variant, rows))
        plot_inputs[test_id] = variant_rows
    return plot_inputs


def load_plot_inputs(run_dir: Path) -> Dict[str, Sequence[tuple[VariantSpec, Sequence[Dict[str, str]]]]]:
    plot_inputs: Dict[str, Sequence[tuple[VariantSpec, Sequence[Dict[str, str]]]]] = {}
    for test_id in TEST_ORDER:
        test_spec = TEST_SPECS[test_id]
        variant_rows: List[tuple[VariantSpec, Sequence[Dict[str, str]]]] = []
        for variant in test_spec.variants:
            csv_path = run_dir / test_id / f"{variant.label}.raw.csv"
            if not csv_path.is_file():
                raise FileNotFoundError(f"missing cached variant CSV: {csv_path}")
            rows = [row for row in bc.read_raw_csv(csv_path) if solver_line_key(row) == "xorcle"]
            variant_rows.append((variant, rows))
        plot_inputs[test_id] = variant_rows
    return plot_inputs


def main(argv: Sequence[str]) -> int:
    args = parse_args(argv)
    tests_root = Path(args.tests_root).resolve()
    output_root = Path(args.output_root).resolve()
    cache_root = Path(args.cache_root).resolve()
    run_config = run_config_for_args(args, tests_root)
    plot_path = output_root / PLOT_NAME
    required_relpaths = [MANIFEST_NAME] + [f"{test_id}/{variant.label}.raw.csv" for test_id in TEST_ORDER for variant in TEST_SPECS[test_id].variants]

    if args.replot_only:
        if not bc.run_has_matching_artifacts(output_root, run_config, required_relpaths):
            print(f"Error: no cached run found for replot under {output_root}", file=sys.stderr)
            return 2
        render_plot(load_plot_inputs(output_root), plot_path)
        print(f"[heuristics] wrote plot {plot_path}")
        return 0

    if bc.run_has_matching_artifacts(output_root, run_config, required_relpaths) and not args.force_rerun:
        plot_inputs = load_plot_inputs(output_root)
        print(f"[heuristics] reusing cached benchmark data from {output_root}")
    else:
        output_root.mkdir(parents=True, exist_ok=True)
        bc.write_run_config(output_root / "run_config.json", run_config)
        plot_inputs = execute_benchmark(args, tests_root=tests_root, run_dir=output_root, cache_root=cache_root)
        print(f"[heuristics] wrote manifest {output_root / MANIFEST_NAME}")

    render_plot(plot_inputs, plot_path)
    print(f"[heuristics] wrote plot {plot_path}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main(sys.argv[1:]))
    except (FileNotFoundError, RuntimeError, ValueError) as exc:
        print(f"Error: {exc}", file=sys.stderr)
        raise SystemExit(2)
