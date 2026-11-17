"""The line styles and marker sizes the three experiments' figures share."""

from __future__ import annotations

import functools

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

#: label -> (colour, line style, end marker, filled): the Okabe-Ito colours, Xorcle black; an older
#: version is dashed in its newer version's colour, with its marker hollow.
STYLES = {
    "Kissat 4.0.4": ("#0072B2", "-", "o", True),
    "CaDiCaL 3.0.1": ("#D55E00", "-", "s", True),
    "CMS 5.17.0": ("#009E73", "-", "^", True),
    "CMS 5.13.0": ("#009E73", "--", "^", False),
    "Xorricane 0.4.2.11": ("#CC79A7", "-", "D", True),
    "2-Xornado 1.0.2": ("#E69F00", "-", "v", True),
    "Xorcle 0.1": ("#000000", "--", "*", False),
    "Xorcle 0.2": ("#000000", "-", "*", True),
}


#: The triangles' marker size; every other end marker is sized to cover the same area, its stroke included.
TRIANGLE_SIZE = 6
#: Every text in the figure: titles, axis labels, tick labels and the legend.
FONT_SIZE = 10
MARKER_EDGE_WIDTH = 1.2
#: Resolution at which a marker's covered area is measured; Agg rounds marker sizes to whole pixels.
_INK_DPI = 2400


@functools.cache
def ink_area(marker: str, size: float) -> float:
    """Area in pt^2 that a filled marker covers as Agg draws it, stroke and joins included."""
    fig = plt.figure(figsize=(0.25, 0.25), dpi=_INK_DPI)
    ax = fig.add_axes((0, 0, 1, 1))
    ax.set_axis_off()
    ax.set_xlim(-1, 1)
    ax.set_ylim(-1, 1)
    ax.plot([0], [0], marker=marker, markersize=size, markeredgewidth=MARKER_EDGE_WIDTH, color="black",
            linestyle="none")
    fig.canvas.draw()
    rgb = np.asarray(fig.canvas.buffer_rgba())[..., :3].astype(float)
    plt.close(fig)
    return float((1 - rgb.mean(axis=2) / 255).sum()) * (72 / _INK_DPI) ** 2


@functools.cache
def marker_size(marker: str) -> float:
    """The marker size at which `marker` covers the area of a triangle at `TRIANGLE_SIZE`."""
    if marker in ("^", "v"):
        return TRIANGLE_SIZE
    target, lo, hi = ink_area("^", TRIANGLE_SIZE), 1.0, 3.0 * TRIANGLE_SIZE
    while hi - lo > 72 / _INK_DPI:
        mid = (lo + hi) / 2
        lo, hi = (mid, hi) if ink_area(marker, mid) < target else (lo, mid)
    return (lo + hi) / 2
