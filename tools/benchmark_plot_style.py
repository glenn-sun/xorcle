from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import Any

UNIFIED_FONT_SIZE = 8.0
UNIFIED_DPI = 300
PLOT_AREA_WIDTH = 2
PLOT_AREA_HEIGHT = 1.5
FIGURE_LEFT = 0.80
FIGURE_RIGHT = 0.20
FIGURE_BOTTOM = 0.55
FIGURE_TOP = 0.45
LEGEND_GAP = 0.20
LEGEND_WIDTH = 2.40

SERIES_COLORS = (
    "#1f77b4",
    "#d62728",
    "#2ca02c",
    "#ff7f0e",
    "#8c564b",
    "#9467bd",
    "#17becf",
)

SERIES_LINESTYLES = (
    "-",
    "--",
    ":",
    "-.",
    (0, (3, 1, 1, 1)),
    (0, (5, 2)),
    (0, (1, 1)),
)

SERIES_MARKERS = ("o", "s", "^", "D", "v", "X", "P")


@dataclass(frozen=True)
class PlotSeriesStyle:
    color: str
    linestyle: Any
    marker: str


def series_style(index: int, *, color_override: str | None = None) -> PlotSeriesStyle:
    return PlotSeriesStyle(
        color=color_override or SERIES_COLORS[index % len(SERIES_COLORS)],
        linestyle=SERIES_LINESTYLES[index % len(SERIES_LINESTYLES)],
        marker=SERIES_MARKERS[index % len(SERIES_MARKERS)],
    )


def apply_common_axes_style(ax: Any, *, xlabel: str, ylabel: str, title: str, font_size: float = UNIFIED_FONT_SIZE) -> None:
    ax.set_xlabel(xlabel, fontsize=font_size)
    ax.set_ylabel(ylabel, fontsize=font_size)
    ax.set_title(title, fontsize=font_size)
    ax.tick_params(axis="both", which="major", labelsize=font_size)
    ax.grid(True, which="both", linestyle="--", linewidth=0.5, alpha=0.35)


def place_right_legend(ax: Any, *, font_size: float = UNIFIED_FONT_SIZE) -> Any:
    return ax.legend(
        fontsize=font_size,
        frameon=False,
        loc="center left",
        bbox_to_anchor=(1.02, 0.5),
        borderaxespad=0.0,
    )


def unified_figure_geometry() -> tuple[tuple[float, float], tuple[float, float, float, float]]:
    fig_width = FIGURE_LEFT + PLOT_AREA_WIDTH + LEGEND_GAP + LEGEND_WIDTH + FIGURE_RIGHT
    fig_height = FIGURE_BOTTOM + PLOT_AREA_HEIGHT + FIGURE_TOP
    left = FIGURE_LEFT / fig_width
    bottom = FIGURE_BOTTOM / fig_height
    width = PLOT_AREA_WIDTH / fig_width
    height = PLOT_AREA_HEIGHT / fig_height
    return (fig_width, fig_height), (left, bottom, width, height)


def create_unified_figure(plt_module: Any) -> tuple[Any, Any]:
    figsize, axes_rect = unified_figure_geometry()
    fig = plt_module.figure(figsize=figsize)
    ax = fig.add_axes(axes_rect)
    return fig, ax


def finalize_figure(fig: Any, output: Path, *, dpi: int = UNIFIED_DPI) -> None:
    fig.savefig(output, dpi=dpi)
