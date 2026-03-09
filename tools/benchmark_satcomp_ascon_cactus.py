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
from benchmark_orchestration import DEFAULT_SOLVER_LINES
from benchmark_orchestration import SOLVER_DISPLAY
from benchmark_orchestration import format_progress_line
from benchmark_orchestration import load_physical_families
from benchmark_orchestration import manifest_rows_for_grouped_instances
from benchmark_orchestration import sample_instances
from benchmark_orchestration import sample_key_for
from benchmark_orchestration import should_report_progress
from benchmark_orchestration import solver_line_key
from benchmark_plot_style import UNIFIED_DPI, UNIFIED_FONT_SIZE
from benchmark_plot_style import paper_layout_axes_rect
from benchmark_plot_style import paper_layout_figure_size
from benchmark_plot_style import paper_layout_legend_anchor
from benchmark_plot_style import two_panel_layout
from plot_cactus import SeriesData, draw_cactus_axes, series_points
from tool_common import repo_root

BENCHMARK_KIND = "satcomp_ascon_cactus"
SATCOMP_RAW_NAME = "sat_comp.raw.csv"
ASCON_RAW_NAME = "ascon.raw.csv"
MANIFEST_NAME = "selected_instances.csv"
PLOT_NAME = "satcomp_ascon_cactus.png"


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
    parser = argparse.ArgumentParser(description="SAT competition / Ascon cactus plots for all solvers.")
    parser.add_argument("--tests-root", default=str(root / "tests"))
    parser.add_argument("--output-root", default=str(root / "benchmarks" / "satcomp_ascon_cactus"))
    parser.add_argument("--cache-root", default=str(root / "benchmarks" / "cache"))
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--timeout", type=float, default=5.0)
    parser.add_argument("--memory-limit-gb", type=float, default=bc.DEFAULT_BENCHMARK_MEMORY_LIMIT_GB)
    parser.add_argument("--satcomp-count", type=int, default=0, help="0 means all SAT competition instances.")
    parser.add_argument("--ascon-count", type=int, default=0, help="0 means all Ascon instances.")
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--saveconverted", action="store_true")
    parser.add_argument("--force-rerun", action="store_true")
    parser.add_argument("--replot-only", action="store_true")
    parser.add_argument("--solver-bin", default=str(root / "bin" / "xorcle"))
    parser.add_argument("--2xornado-bin", dest="xornado_bin", default=str(root / "external" / "2xornado"))
    parser.add_argument("--kissat-bin", default=str(root / "external" / "kissat"))
    parser.add_argument("--cms-bin", default=str(root / "external" / "cryptominisat5"))
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
        kissat_bin=str(Path(args.kissat_bin).resolve()),
        cms_bin=str(Path(args.cms_bin).resolve()),
        xornado_bin=str(Path(args.xornado_bin).resolve()),
        csv_path="",
        exp_cnf=False,
        solvers=list(DEFAULT_SOLVER_LINES),
        saveconverted=bool(args.saveconverted),
    )


def run_config_for_args(args: argparse.Namespace, tests_root: Path) -> Dict[str, object]:
    return {
        "benchmark_kind": BENCHMARK_KIND,
        "tests_root": str(tests_root),
        "families": ["sat-comp", "ascon"],
        "satcomp_count": parse_count(args.satcomp_count),
        "ascon_count": parse_count(args.ascon_count),
        "seed": int(args.seed),
        "timeout": float(args.timeout),
        "memory_limit_gb": float(args.memory_limit_gb),
        "solver_bin": str(Path(args.solver_bin).resolve()),
        "kissat_bin": str(Path(args.kissat_bin).resolve()),
        "cms_bin": str(Path(args.cms_bin).resolve()),
        "xornado_bin": str(Path(args.xornado_bin).resolve()),
        "saveconverted": bool(args.saveconverted),
        "solvers": list(DEFAULT_SOLVER_LINES),
    }


def select_corpora(args: argparse.Namespace, tests_root: Path) -> Dict[str, List[object]]:
    families = load_physical_families(tests_root, ["sat-comp", "ascon"])
    selected: Dict[str, List[object]] = {}
    satcomp_count = parse_count(args.satcomp_count)
    ascon_count = parse_count(args.ascon_count)
    for family_name, count in (("sat-comp", satcomp_count), ("ascon", ascon_count)):
        records = families[family_name]
        if count is None:
            selected[family_name] = sample_instances(
                records,
                count=len(records),
                seed=args.seed,
                cohort_id=f"family:{family_name}",
            )
        else:
            selected[family_name] = sample_instances(
                records,
                count=count,
                seed=args.seed,
                cohort_id=f"family:{family_name}",
            )
    return selected


def build_series(rows: Sequence[Dict[str, str]]) -> List[SeriesData]:
    return [
        SeriesData(label=SOLVER_DISPLAY[solver], rows=[row for row in rows if solver_line_key(row) == solver])
        for solver in DEFAULT_SOLVER_LINES
    ]


def legend_labels_for_pair(satcomp_rows: Sequence[Dict[str, str]], ascon_rows: Sequence[Dict[str, str]]) -> List[str]:
    satcomp_series = build_series(satcomp_rows)
    ascon_series = build_series(ascon_rows)
    labels: List[str] = []
    for satcomp_item, ascon_item in zip(satcomp_series, ascon_series):
        _, satcomp_solved, satcomp_total = series_points(
            satcomp_item.rows,
            aggregate_per_file="median",
            include_timeouts=False,
            metric="runtime",
        )
        _, ascon_solved, ascon_total = series_points(
            ascon_item.rows,
            aggregate_per_file="median",
            include_timeouts=False,
            metric="runtime",
        )
        labels.append(f"{satcomp_item.label}\n({satcomp_solved}/{satcomp_total} SC, {ascon_solved}/{ascon_total} Ascon)")
    return labels


def render_plot(satcomp_rows: Sequence[Dict[str, str]], ascon_rows: Sequence[Dict[str, str]], output_path: Path) -> None:
    plt = import_matplotlib_pyplot()
    layout = two_panel_layout(panel_height=1.75, legend_width=1.95)
    fig = plt.figure(figsize=paper_layout_figure_size(layout))
    axes = [
        fig.add_axes(paper_layout_axes_rect(layout, row=0, col=0)),
        fig.add_axes(paper_layout_axes_rect(layout, row=0, col=1)),
    ]
    satcomp_series = build_series(satcomp_rows)
    ascon_series = build_series(ascon_rows)

    draw_cactus_axes(
        axes[0],
        satcomp_series,
        aggregate_per_file="median",
        include_timeouts=False,
        metric="runtime",
        line_mode="line",
        black_white=False,
        title="SAT Competition",
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
        title="Ascon",
        xlabel="Instances solved",
        ylabel="Time (s)",
        x_scale="linear",
        y_scale="linear",
        x_ticks=[],
        y_ticks=[],
        font_size=UNIFIED_FONT_SIZE,
        show_counts=False,
    )

    fig.legend(
        list(axes[0].get_lines()),
        legend_labels_for_pair(satcomp_rows, ascon_rows),
        fontsize=UNIFIED_FONT_SIZE,
        frameon=False,
        loc="center left",
        bbox_to_anchor=paper_layout_legend_anchor(layout),
        borderaxespad=0.0,
        handleheight=1.4,
        labelspacing=0.8,
    )

    output_path.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(output_path, dpi=UNIFIED_DPI)
    plt.close(fig)


def write_manifest(path: Path, grouped_records) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as csv_file:
        writer = csv.DictWriter(
            csv_file,
            fieldnames=["family", "sample_index", "sample_key", "file", "input_format", "attempt", "n", "k", "h", "lift_k"],
        )
        writer.writeheader()
        writer.writerows(manifest_rows_for_grouped_instances(grouped_records))


def execute_benchmark(args: argparse.Namespace, *, tests_root: Path, run_dir: Path, cache_root: Path) -> tuple[List[Dict[str, str]], List[Dict[str, str]]]:
    converter_script = repo_root() / "tools" / "xnf_convert.py"
    selected_records = select_corpora(args, tests_root)
    write_manifest(run_dir / MANIFEST_NAME, selected_records)
    rows_by_family: Dict[str, List[Dict[str, str]]] = {"sat-comp": [], "ascon": []}
    sample_keys = {
        str(record.file.resolve()): sample_key_for(family=family, index=index, file_path=record.file)
        for family, records in selected_records.items()
        for index, record in enumerate(records, start=1)
    }
    file_to_family = {
        str(record.file.resolve()): family
        for family, records in selected_records.items()
        for record in records
    }
    total_tasks = len(sample_keys)
    progress_every = max(1, total_tasks // 40)
    progress_done = 0
    start_s = time.perf_counter()

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
                    family=file_to_family.get(str(file_path.resolve()), ""),
                    file=file_path.name,
                    reused="yes" if reused else "no",
                )
            )

    for family, records in selected_records.items():
        files_by_format: Dict[str, List[Path]] = {}
        for record in records:
            files_by_format.setdefault(record.input_format, []).append(record.file)
        family_rows: List[Dict[str, str]] = []
        for input_format, selected_files in files_by_format.items():
            family_rows.extend(
                bc.ensure_benchmark_file_coverage(
                    selected_files,
                    make_runner_args(args, input_format=input_format),
                    [],
                    converter_script,
                    jobs=args.jobs,
                    cache_root=cache_root,
                    force_rerun=bool(args.force_rerun),
                    progress_callback=report_progress,
                )
            )
        for row in family_rows:
            row["sample_key"] = sample_keys[str(Path(row["file"]).resolve())]
        rows_by_family[family] = family_rows

    for family in rows_by_family:
        rows_by_family[family].sort(
            key=lambda row: (
                row.get("sample_key", ""),
                row.get("solver_config", ""),
                row.get("backend_input_kind", ""),
                row.get("solver", ""),
            )
        )
    bc.write_raw_csv(run_dir / SATCOMP_RAW_NAME, rows_by_family["sat-comp"])
    bc.write_raw_csv(run_dir / ASCON_RAW_NAME, rows_by_family["ascon"])
    return rows_by_family["sat-comp"], rows_by_family["ascon"]


def main(argv: Sequence[str]) -> int:
    args = parse_args(argv)
    tests_root = Path(args.tests_root).resolve()
    output_root = Path(args.output_root).resolve()
    cache_root = Path(args.cache_root).resolve()
    run_config = run_config_for_args(args, tests_root)
    plot_path = output_root / PLOT_NAME

    if args.replot_only:
        if not bc.run_has_matching_artifacts(output_root, run_config, [SATCOMP_RAW_NAME, ASCON_RAW_NAME, MANIFEST_NAME]):
            print(f"Error: no cached run found for replot under {output_root}", file=sys.stderr)
            return 2
        satcomp_rows = bc.read_raw_csv(output_root / SATCOMP_RAW_NAME)
        ascon_rows = bc.read_raw_csv(output_root / ASCON_RAW_NAME)
        render_plot(satcomp_rows, ascon_rows, plot_path)
        print(f"[satcomp-ascon] wrote plot {plot_path}")
        return 0

    if bc.run_has_matching_artifacts(output_root, run_config, [SATCOMP_RAW_NAME, ASCON_RAW_NAME, MANIFEST_NAME]) and not args.force_rerun:
        satcomp_rows = bc.read_raw_csv(output_root / SATCOMP_RAW_NAME)
        ascon_rows = bc.read_raw_csv(output_root / ASCON_RAW_NAME)
        print(f"[satcomp-ascon] reusing cached benchmark data from {output_root}")
    else:
        output_root.mkdir(parents=True, exist_ok=True)
        bc.write_run_config(output_root / "run_config.json", run_config)
        satcomp_rows, ascon_rows = execute_benchmark(args, tests_root=tests_root, run_dir=output_root, cache_root=cache_root)
        print(f"[satcomp-ascon] wrote raw data {output_root / SATCOMP_RAW_NAME}")
        print(f"[satcomp-ascon] wrote raw data {output_root / ASCON_RAW_NAME}")
        print(f"[satcomp-ascon] wrote manifest {output_root / MANIFEST_NAME}")

    render_plot(satcomp_rows, ascon_rows, plot_path)
    print(f"[satcomp-ascon] wrote plot {plot_path}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main(sys.argv[1:]))
    except (FileNotFoundError, RuntimeError, ValueError) as exc:
        print(f"Error: {exc}", file=sys.stderr)
        raise SystemExit(2)
