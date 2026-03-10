#!/usr/bin/env python3
from __future__ import annotations

import argparse
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Sequence

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


@dataclass(frozen=True)
class CorpusPlotSpec:
    input_name: str
    output_name: str
    display_name: str
    runtime_title: str
    decisions_title: str


CORPUS_PLOTS = (
    CorpusPlotSpec(
        input_name="sat_comp.raw.csv",
        output_name="sat_comp_cactus.png",
        display_name="SAT Competition '25",
        runtime_title="SAT Competition '25 - Runtime",
        decisions_title="SAT Competition '25 - Decisions",
    ),
    CorpusPlotSpec(
        input_name="ascon.raw.csv",
        output_name="ascon_cactus.png",
        display_name="Ascon-128",
        runtime_title="Ascon-128 - Runtime",
        decisions_title="Ascon-128 - Decisions",
    ),
)


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    root = repo_root()
    parser = argparse.ArgumentParser(
        description="Render temporary standalone SAT competition and Ascon cactus plots from cached artifacts."
    )
    parser.add_argument(
        "--artifacts-root",
        default=str(root / "benchmarks" / "satcomp_ascon_cactus"),
        help="Directory containing sat_comp.raw.csv and ascon.raw.csv.",
    )
    parser.add_argument(
        "--output-root",
        default=str(root / "benchmarks" / "satcomp_ascon_cactus" / "single_cactus"),
        help="Directory to write the temporary standalone cactus plots.",
    )
    return parser.parse_args(argv)


def render_corpus_plot(rows: Sequence[Dict[str, str]], output_path: Path, spec: CorpusPlotSpec) -> None:
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

    fig.legend(
        list(axes[0].get_lines()),
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

    output_root.mkdir(parents=True, exist_ok=True)
    for spec in CORPUS_PLOTS:
        input_path = artifacts_root / spec.input_name
        if not input_path.is_file():
            raise FileNotFoundError(f"missing cached raw CSV: {input_path}")
        rows = bc.read_raw_csv(input_path)
        if not rows:
            raise RuntimeError(f"cached raw CSV is empty: {input_path}")
        output_path = output_root / spec.output_name
        render_corpus_plot(rows, output_path, spec)
        print(f"[satcomp-ascon-single] wrote plot {output_path} for {spec.display_name} ({len(rows)} rows)")

    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main(sys.argv[1:]))
    except (FileNotFoundError, RuntimeError, ValueError) as exc:
        print(f"Error: {exc}", file=sys.stderr)
        raise SystemExit(2)
