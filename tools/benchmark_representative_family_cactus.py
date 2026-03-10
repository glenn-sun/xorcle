#!/usr/bin/env python3
from __future__ import annotations

import argparse
import csv
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Sequence, Set

import benchmark_core as bc
from benchmark_plot_style import UNIFIED_DPI, UNIFIED_FONT_SIZE
from benchmark_plot_style import paper_legend_style
from benchmark_plot_style import paper_layout_axes_rect
from benchmark_plot_style import paper_layout_figure_size
from benchmark_plot_style import paper_layout_legend_anchor
from benchmark_plot_style import representative_cactus_layout
from benchmark_representative_cactus import build_series
from benchmark_representative_cactus import import_matplotlib_pyplot
from benchmark_representative_cactus import legend_labels
from plot_cactus import draw_cactus_axes
from tool_common import repo_root

RAW_CSV_NAME = "representative.raw.csv"
MANIFEST_NAME = "selected_instances.csv"


@dataclass(frozen=True)
class FamilyPlotSpec:
    family: str
    output_name: str
    display_name: str
    runtime_title: str
    decisions_title: str


FAMILY_PLOTS = (
    FamilyPlotSpec(
        family="generated/random_kxnf_n",
        output_name="random_xnf_cactus.png",
        display_name="random XNFs",
        runtime_title="Random k-XNFs - Runtime",
        decisions_title="Random k-XNFs - Decisions",
    ),
    FamilyPlotSpec(
        family="generated/restricted_kxnf_n",
        output_name="restricted_random_xnf_cactus.png",
        display_name="restricted random XNFs",
        runtime_title="Random Restricted k-XNFs - Runtime",
        decisions_title="Random Restricted k-XNFs - Decisions",
    ),
    FamilyPlotSpec(
        family="generated/pebbling_lifted_h_k",
        output_name="pebbling_cactus.png",
        display_name="pebbling formulas",
        runtime_title="Lifted Pebbling - Runtime",
        decisions_title="Lifted Pebbling - Decisions",
    ),
)


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    root = repo_root()
    parser = argparse.ArgumentParser(
        description="Render temporary representative cactus plots filtered to selected benchmark families."
    )
    parser.add_argument(
        "--artifacts-root",
        default=str(root / "benchmarks" / "representative_cactus"),
        help="Directory containing representative.raw.csv and selected_instances.csv.",
    )
    parser.add_argument(
        "--output-root",
        default=str(root / "benchmarks" / "representative_cactus" / "family_cactus"),
        help="Directory to write the filtered family cactus plots.",
    )
    return parser.parse_args(argv)


def read_manifest_rows(path: Path) -> List[Dict[str, str]]:
    with path.open("r", encoding="utf-8", newline="") as csv_file:
        return list(csv.DictReader(csv_file))


def sample_keys_for_family(manifest_rows: Sequence[Dict[str, str]], family: str) -> Set[str]:
    sample_keys: Set[str] = set()
    for row in manifest_rows:
        if (row.get("family") or "").strip() != family:
            continue
        sample_key = (row.get("sample_key") or "").strip()
        if not sample_key:
            raise RuntimeError(f"{MANIFEST_NAME}: missing sample_key for family {family}")
        sample_keys.add(sample_key)
    if not sample_keys:
        raise RuntimeError(f"{MANIFEST_NAME}: no selected instances found for family {family}")
    return sample_keys


def filter_rows_for_sample_keys(rows: Sequence[Dict[str, str]], sample_keys: Set[str], family: str) -> List[Dict[str, str]]:
    filtered = [row for row in rows if (row.get("sample_key") or "").strip() in sample_keys]
    present_keys = {(row.get("sample_key") or "").strip() for row in filtered}
    missing_keys = sorted(sample_keys - present_keys)
    if missing_keys:
        preview = ", ".join(missing_keys[:5])
        suffix = "" if len(missing_keys) <= 5 else ", ..."
        raise RuntimeError(f"{RAW_CSV_NAME}: missing benchmark rows for family {family}: {preview}{suffix}")
    if not filtered:
        raise RuntimeError(f"{RAW_CSV_NAME}: no benchmark rows found for family {family}")
    return filtered


def render_family_plot(rows: Sequence[Dict[str, str]], output_path: Path, spec: FamilyPlotSpec) -> None:
    plt = import_matplotlib_pyplot()
    layout = representative_cactus_layout()
    legend_style = paper_legend_style()
    fig = plt.figure(figsize=paper_layout_figure_size(layout))
    axes = [
        fig.add_axes(paper_layout_axes_rect(layout, row=0, col=0)),
        fig.add_axes(paper_layout_axes_rect(layout, row=0, col=1)),
    ]
    series = build_series(rows)

    draw_cactus_axes(
        axes[0],
        series,
        aggregate_per_file="median",
        include_timeouts=False,
        metric="runtime",
        line_mode="line",
        black_white=False,
        title=spec.runtime_title,
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
        series,
        aggregate_per_file="median",
        include_timeouts=False,
        metric="decisions",
        line_mode="line",
        black_white=False,
        title=spec.decisions_title,
        xlabel="Instances solved",
        ylabel="Decisions",
        x_scale="linear",
        y_scale="log",
        x_ticks=[],
        y_ticks=[],
        font_size=UNIFIED_FONT_SIZE,
        show_counts=False,
    )
    axes[1].minorticks_off()
    axes[1].grid(False, which="minor")

    handles = list(axes[0].get_lines())
    fig.legend(
        handles,
        legend_labels(series),
        fontsize=UNIFIED_FONT_SIZE,
        frameon=False,
        loc="center left",
        bbox_to_anchor=paper_layout_legend_anchor(layout),
        borderaxespad=legend_style.borderaxespad,
        handleheight=legend_style.handleheight,
        labelspacing=legend_style.labelspacing,
    )

    output_path.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(output_path, dpi=UNIFIED_DPI)
    plt.close(fig)


def main(argv: Sequence[str]) -> int:
    args = parse_args(argv)
    artifacts_root = Path(args.artifacts_root).resolve()
    output_root = Path(args.output_root).resolve()

    manifest_path = artifacts_root / MANIFEST_NAME
    raw_csv_path = artifacts_root / RAW_CSV_NAME
    if not manifest_path.is_file():
        raise FileNotFoundError(f"missing representative manifest: {manifest_path}")
    if not raw_csv_path.is_file():
        raise FileNotFoundError(f"missing representative raw CSV: {raw_csv_path}")

    manifest_rows = read_manifest_rows(manifest_path)
    raw_rows = bc.read_raw_csv(raw_csv_path)

    output_root.mkdir(parents=True, exist_ok=True)
    for spec in FAMILY_PLOTS:
        sample_keys = sample_keys_for_family(manifest_rows, spec.family)
        filtered_rows = filter_rows_for_sample_keys(raw_rows, sample_keys, spec.family)
        output_path = output_root / spec.output_name
        render_family_plot(filtered_rows, output_path, spec)
        print(
            f"[representative-family] wrote plot {output_path} "
            f"for {spec.display_name} ({len(sample_keys)} instances, {len(filtered_rows)} rows)"
        )

    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main(sys.argv[1:]))
    except (FileNotFoundError, RuntimeError, ValueError) as exc:
        print(f"Error: {exc}", file=sys.stderr)
        raise SystemExit(2)
