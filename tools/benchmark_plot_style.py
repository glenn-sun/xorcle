from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import Any

UNIFIED_FONT_SIZE = 8.0
UNIFIED_DPI = 300
PAPER_FIGURE_WIDTH = 8
PAPER_MARGIN_LEFT = 0.5
PAPER_MARGIN_RIGHT = 0.25
PAPER_MARGIN_BOTTOM = 0.5
PAPER_MARGIN_TOP = 0.25
PAPER_GUTTER_X = 0.75
PAPER_GUTTER_Y = 0
PAPER_LEGEND_GAP = 0.2
PAPER_LEGEND_WIDTH = 1.5
PAPER_LINE_WIDTH = 1.5
PAPER_MARKER_SIZE = 14.0
PAPER_SCATTER_ALPHA = 0.75
PAPER_LEGEND_LABELSPACING = 0.8
PAPER_LEGEND_HANDLEHEIGHT = 1.4
PAPER_LEGEND_BORDERAXESPAD = 0.0

SERIES_COLORS = (
    "#1f77b4",
    "#d62728",
    "#2ca02c",
    "#ff7f0e",
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
)

SERIES_MARKERS = ("o", "s", "^", "D", "v", "X")


@dataclass(frozen=True)
class PlotSeriesStyle:
    color: str
    linestyle: Any
    marker: str


@dataclass(frozen=True)
class PaperFigureLayout:
    rows: int
    cols: int
    panel_height: float
    legend_width: float = PAPER_LEGEND_WIDTH
    legend_gap: float = PAPER_LEGEND_GAP
    margin_left: float = PAPER_MARGIN_LEFT
    margin_right: float = PAPER_MARGIN_RIGHT
    margin_bottom: float = PAPER_MARGIN_BOTTOM
    margin_top: float = PAPER_MARGIN_TOP
    gutter_x: float = PAPER_GUTTER_X
    gutter_y: float = PAPER_GUTTER_Y


@dataclass(frozen=True)
class PanelBlockLayout:
    rows: int
    cols: int
    panel_height: float
    per_panel_legend_width: float
    per_panel_legend_gap: float = PAPER_LEGEND_GAP
    margin_left: float = PAPER_MARGIN_LEFT
    margin_right: float = PAPER_MARGIN_RIGHT
    margin_bottom: float = PAPER_MARGIN_BOTTOM
    margin_top: float = PAPER_MARGIN_TOP
    gutter_x: float = PAPER_GUTTER_X
    gutter_y: float = PAPER_GUTTER_Y


@dataclass(frozen=True)
class LegendStyle:
    labelspacing: float = PAPER_LEGEND_LABELSPACING
    handleheight: float = PAPER_LEGEND_HANDLEHEIGHT
    borderaxespad: float = PAPER_LEGEND_BORDERAXESPAD


@dataclass(frozen=True)
class TseitinFitStyle:
    color: str
    linestyle: Any


@dataclass(frozen=True)
class TseitinVisualStyle:
    scatter_alpha: float = PAPER_SCATTER_ALPHA
    vertex_fit: TseitinFitStyle = TseitinFitStyle(color="#222222", linestyle="--")
    degree_fit: TseitinFitStyle = TseitinFitStyle(color="#222222", linestyle=":")


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


def apply_log_tick_format(ax: Any, *, x_scale: str, y_scale: str) -> None:
    try:
        from matplotlib.ticker import LogFormatterMathtext
    except ModuleNotFoundError as exc:  # pragma: no cover
        raise SystemExit(f"Error: missing dependency '{exc.name}'. Install matplotlib to run plotting.")

    if x_scale == "log":
        ax.xaxis.set_major_formatter(LogFormatterMathtext(base=10))
    if y_scale == "log":
        ax.yaxis.set_major_formatter(LogFormatterMathtext(base=10))


def place_right_legend(ax: Any, *, font_size: float = UNIFIED_FONT_SIZE) -> Any:
    return ax.legend(
        fontsize=font_size,
        frameon=False,
        loc="center left",
        bbox_to_anchor=(1.02, 0.5),
        borderaxespad=0.0,
    )


def paper_layout_figure_size(layout: PaperFigureLayout) -> tuple[float, float]:
    panel_width = (
        PAPER_FIGURE_WIDTH
        - layout.margin_left
        - layout.margin_right
        - layout.legend_gap
        - layout.legend_width
        - ((layout.cols - 1) * layout.gutter_x)
    ) / layout.cols
    if panel_width <= 0.0:
        raise ValueError("paper layout leaves no room for plot panels")
    figure_height = (
        layout.margin_bottom
        + layout.margin_top
        + (layout.rows * layout.panel_height)
        + ((layout.rows - 1) * layout.gutter_y)
    )
    return PAPER_FIGURE_WIDTH, figure_height


def paper_layout_axes_rect(layout: PaperFigureLayout, *, row: int, col: int) -> tuple[float, float, float, float]:
    figure_width, figure_height = paper_layout_figure_size(layout)
    panel_width = (
        PAPER_FIGURE_WIDTH
        - layout.margin_left
        - layout.margin_right
        - layout.legend_gap
        - layout.legend_width
        - ((layout.cols - 1) * layout.gutter_x)
    ) / layout.cols
    left_in = layout.margin_left + (col * (panel_width + layout.gutter_x))
    bottom_in = layout.margin_bottom + ((layout.rows - 1 - row) * (layout.panel_height + layout.gutter_y))
    return (
        left_in / figure_width,
        bottom_in / figure_height,
        panel_width / figure_width,
        layout.panel_height / figure_height,
    )


def paper_layout_legend_anchor(layout: PaperFigureLayout) -> tuple[float, float]:
    figure_width, _ = paper_layout_figure_size(layout)
    panel_width = (
        PAPER_FIGURE_WIDTH
        - layout.margin_left
        - layout.margin_right
        - layout.legend_gap
        - layout.legend_width
        - ((layout.cols - 1) * layout.gutter_x)
    ) / layout.cols
    legend_left_in = layout.margin_left + (layout.cols * panel_width) + ((layout.cols - 1) * layout.gutter_x) + layout.legend_gap
    return legend_left_in / figure_width, 0.5


def two_panel_layout(*, panel_height: float = 1.7, legend_width: float = PAPER_LEGEND_WIDTH, margin_left: float = PAPER_MARGIN_LEFT) -> PaperFigureLayout:
    return PaperFigureLayout(rows=1, cols=2, panel_height=panel_height, legend_width=legend_width, margin_left=margin_left)


def four_panel_layout(*, panel_height: float = 1.55, legend_width: float = PAPER_LEGEND_WIDTH) -> PaperFigureLayout:
    return PaperFigureLayout(rows=2, cols=2, panel_height=panel_height, legend_width=legend_width)


def paper_legend_style() -> LegendStyle:
    return LegendStyle()


def representative_cactus_layout() -> PaperFigureLayout:
    return two_panel_layout(panel_height=1.75, legend_width=1.5)


def satcomp_ascon_cactus_layout() -> PaperFigureLayout:
    return two_panel_layout(panel_height=1.75, legend_width=1.5)


def tseitin_regression_layout() -> PaperFigureLayout:
    return two_panel_layout(panel_height=1.75, legend_width=1.25, margin_left=0.75)


def xorcle_heuristics_layout() -> PanelBlockLayout:
    return PanelBlockLayout(
        rows=2,
        cols=2,
        panel_height=1.75,
        per_panel_legend_width=1.2,
        per_panel_legend_gap=0.2,
        gutter_x=0.6,
        gutter_y=0.8,
    )


def tseitin_visual_style() -> TseitinVisualStyle:
    return TseitinVisualStyle()


def block_layout_figure_size(layout: PanelBlockLayout) -> tuple[float, float]:
    block_width = (
        PAPER_FIGURE_WIDTH
        - layout.margin_left
        - layout.margin_right
        - ((layout.cols - 1) * layout.gutter_x)
    ) / layout.cols
    panel_width = block_width - layout.per_panel_legend_gap - layout.per_panel_legend_width
    if panel_width <= 0.0:
        raise ValueError("block layout leaves no room for plot panels")
    figure_height = (
        layout.margin_bottom
        + layout.margin_top
        + (layout.rows * layout.panel_height)
        + ((layout.rows - 1) * layout.gutter_y)
    )
    return PAPER_FIGURE_WIDTH, figure_height


def block_layout_axes_rect(layout: PanelBlockLayout, *, row: int, col: int) -> tuple[float, float, float, float]:
    figure_width, figure_height = block_layout_figure_size(layout)
    block_width = (
        PAPER_FIGURE_WIDTH
        - layout.margin_left
        - layout.margin_right
        - ((layout.cols - 1) * layout.gutter_x)
    ) / layout.cols
    panel_width = block_width - layout.per_panel_legend_gap - layout.per_panel_legend_width
    left_in = layout.margin_left + (col * (block_width + layout.gutter_x))
    bottom_in = layout.margin_bottom + ((layout.rows - 1 - row) * (layout.panel_height + layout.gutter_y))
    return (
        left_in / figure_width,
        bottom_in / figure_height,
        panel_width / figure_width,
        layout.panel_height / figure_height,
    )


def block_layout_legend_anchor(layout: PanelBlockLayout, *, row: int, col: int) -> tuple[float, float]:
    figure_width, figure_height = block_layout_figure_size(layout)
    block_width = (
        PAPER_FIGURE_WIDTH
        - layout.margin_left
        - layout.margin_right
        - ((layout.cols - 1) * layout.gutter_x)
    ) / layout.cols
    panel_width = block_width - layout.per_panel_legend_gap - layout.per_panel_legend_width
    left_in = layout.margin_left + (col * (block_width + layout.gutter_x)) + panel_width + layout.per_panel_legend_gap
    bottom_in = layout.margin_bottom + ((layout.rows - 1 - row) * (layout.panel_height + layout.gutter_y))
    return left_in / figure_width, (bottom_in + (layout.panel_height / 2.0)) / figure_height


def unified_figure_geometry() -> tuple[tuple[float, float], tuple[float, float, float, float]]:
    layout = PaperFigureLayout(rows=1, cols=1, panel_height=1.6)
    figsize = paper_layout_figure_size(layout)
    axes_rect = paper_layout_axes_rect(layout, row=0, col=0)
    return figsize, axes_rect


def create_unified_figure(plt_module: Any) -> tuple[Any, Any]:
    figsize, axes_rect = unified_figure_geometry()
    fig = plt_module.figure(figsize=figsize)
    ax = fig.add_axes(axes_rect)
    return fig, ax


def finalize_figure(fig: Any, output: Path, *, dpi: int = UNIFIED_DPI) -> None:
    fig.savefig(output, dpi=dpi)
