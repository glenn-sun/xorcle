#!/usr/bin/env python3
"""Plot median time against n per solver, dense and sparse at k = 2, 3 and 5 (the paper's fig:rand).

    report.py      writes ../results/random/random.png and ../results/random/chains.md


Each point is a solver's median over all instances at one n, SAT and UNSAT pooled; a run without a
verdict counts as above the time limit, and only medians from 1 s up to it are drawn. The line is the
least-squares fit of log median time over n, through the medians from 1 s up to the time limit.
`chains.md` lists, per line and solver, the n at which the chain stopped: the first n at which
at least 8 of its 10 runs gave no verdict.
"""

from __future__ import annotations

import argparse
import collections
import math
import statistics as st
import sys
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.lines import Line2D
from matplotlib.ticker import FuncFormatter, MaxNLocator
from matplotlib.transforms import Bbox

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
import measure
from plot_style import FONT_SIZE, MARKER_EDGE_WIDTH, STYLES, marker_size

LABELS = {"kissat-4.0.4": "Kissat 4.0.4", "cadical-3.0.1": "CaDiCaL 3.0.1", "cms-5.17.0": "CMS 5.17.0",
          "xorricane-0.4.2.11": "Xorricane 0.4.2.11", "xorcle-0.2": "Xorcle 0.2"}
#: (row, column) -> line; rows dense then sparse, columns k = 2, 3 and 5.
PANELS = {(row, col): f"{kind}_k{k}" for row, kind in enumerate(("dense", "sparse"))
          for col, k in enumerate((2, 3, 5))}
#: The points' markers, as a fraction of the cactus plots' end markers.
POINT_SCALE = 0.8


def fit(points: list[tuple[int, float]]) -> tuple[list[int], list[float]] | None:
    """The least-squares line of log median over n through the medians of at least 1 s, at its end n."""
    pts = [(n, math.log(m)) for n, m in points if m >= 1]
    if len(pts) < 3:
        return None
    mx, my = st.mean(n for n, _ in pts), st.mean(y for _, y in pts)
    b = sum((n - mx) * (y - my) for n, y in pts) / sum((n - mx) ** 2 for n, _ in pts)
    xs = [pts[0][0], pts[-1][0]]
    return xs, [math.exp(my + b * (x - mx)) for x in xs]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.parse_args()
    results = measure.RESULTS / "random"
    out = results / "random.png"
    times = collections.defaultdict(list)
    for f in sorted(results.glob("*.jsonl")):
        for r in measure.read_jsonl(f):
            times[(r["line"], r["arm"], r["n"])].append(r["wall_s"] if r["verdict"] in ("SAT", "UNSAT") else math.inf)

    fig, axes = plt.subplots(2, 3, figsize=(9.6, 5.8), sharey=True)
    handles = {}
    for (row, col), line in PANELS.items():
        ax = axes[row, col]
        for arm, label in LABELS.items():
            color, style, marker, _ = STYLES[label]
            pts = sorted((n, st.median(ts)) for (l, a, n), ts in times.items() if l == line and a == arm)
            shown = [(n, m) for n, m in pts if 1 <= m < math.inf]
            mark = dict(marker=marker, markersize=POINT_SCALE * marker_size(marker), markerfacecolor=color,
                        markeredgecolor=color, markeredgewidth=POINT_SCALE * MARKER_EDGE_WIDTH)
            ax.plot([n for n, _ in shown], [m for _, m in shown], linestyle="none", **mark)
            line_pts = fit(shown)
            if line_pts:
                ax.plot(*line_pts, color=color, linestyle=style, linewidth=1.2)
            handles[label] = Line2D([], [], color=color, linestyle=style, linewidth=1.2, **mark)
        kind, k = line.split("_k")
        ax.set_title(f"{kind.capitalize()} {k}-XNF formulas", fontsize=FONT_SIZE)
        ax.set_yscale("log")
        ax.set_ylim(1, 300)
        ax.yaxis.set_major_formatter(FuncFormatter(lambda y, _: f"{y:g}"))
        ax.tick_params(labelsize=FONT_SIZE)
        ax.xaxis.set_major_locator(MaxNLocator(integer=True))
        if row == 1:
            ax.set_xlabel("number of variables", fontsize=FONT_SIZE)
        if col == 0:
            ax.set_ylabel("median time (s)", fontsize=FONT_SIZE)
    stops = collections.defaultdict(int)
    for (line, arm, n), ts in times.items():
        if sum(t == math.inf for t in ts) >= 0.8 * len(ts):
            stops[(line, arm)] = max(stops[(line, arm)], n)
    rows = ["| line | " + " | ".join(LABELS.values()) + " |", "|---|" + "---:|" * len(LABELS)]
    rows += [f"| {line} | " + " | ".join(str(stops.get((line, arm), "—")) for arm in LABELS) + " |"
             for line in PANELS.values()]
    (results / "chains.md").write_text("The n at which each chain stopped (at least 80% of its runs without a verdict).\n\n"
                                       + "\n".join(rows) + "\n")
    fig.legend(handles.values(), handles.keys(), loc="lower center", ncol=len(handles), frameon=False, fontsize=FONT_SIZE)
    fig.tight_layout(rect=(0, 0.06, 1, 1))
    tight = fig.get_tightbbox(fig.canvas.get_renderer())
    fig.savefig(out, dpi=150, bbox_inches=Bbox.from_extents(tight.x0, 0, tight.x1, fig.get_figheight()))
    print(out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
