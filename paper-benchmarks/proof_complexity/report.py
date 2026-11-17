#!/usr/bin/env python3
"""Write the proof-complexity table and figure from `../results/proof_complexity/`.

    report.py

`largest_solved.md` (the paper's tab:pc) gives the largest n each solver solved within the
time limit, per line. `pc.png` (the paper's fig:pc) plots Xorcle v0.2 plain's time and
LRUP(⊕) lines written against the clause count, one column per line, over the points it
solved from the line's `FIT_FROM` up. Proof lines come from the separate proof runs, each proof checked by
`check_lrup_parity`. Each panel's points are fitted by least squares in log-log, and the fitted
exponent is written in the panel's bottom right corner. Where every run past the last solved n ran out of memory, that
stretch of the axis is shaded and marked MEMOUT.
"""

from __future__ import annotations

import collections
import csv
import math
import statistics as st
import sys
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import FuncFormatter, LogLocator, NullFormatter
from matplotlib.transforms import Bbox

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
import measure
from plot_style import FONT_SIZE, MARKER_EDGE_WIDTH, marker_size

RESULTS = measure.RESULTS / "proof_complexity"
TABLE_ARMS = [("kissat-4.0.4", "Kissat 4.0.4"), ("cadical-3.0.1", "CaDiCaL 3.0.1"), ("cms-5.17.0", "CMS 5.17.0"),
              ("xorricane-0.4.2.11", "Xorricane 0.4.2.11"), ("xorcle-0.2", "Xorcle 0.2"),
              ("xorcle-0.2_plain", "Xorcle 0.2 plain")]

#: line -> title.
LINES = {"tseitin_k3": "Tseitin, 3-regular graphs", "perfect_matching_k4": "Perfect matching, 4-regular graphs",
         "bijective_pigeonhole_k1": "Bijective PHP^(n+1)_n"}
XLABEL = "number of clauses (m)"
#: The smallest n each line's panels show and fit. Below it, tseitin's times are within a few tenths of a
#: second of the fixed start-up cost, and perfect_matching's proof-line counts sit on a small-graph plateau.
FIT_FROM = {"tseitin_k3": 1000, "perfect_matching_k4": 301, "bijective_pigeonhole_k1": 9}
ARM = "xorcle-0.2_plain"
#: (record field, y-axis label, symbol in the fitted law).
ROWS = [("wall_s", r"time ($t$, seconds)", "t"), ("proof_lines", r"LRUP($\oplus$) proof lines ($\ell$)", r"\ell")]
#: The fitted law's distance from the panel's bottom edge and from its right edge (the MEMOUT band's, if any), in points.
LAW_OFFSET = (8, 8)
#: The time axis of every panel, up to the time limit.
TIME_LIM = (0.5, 300)
#: The points' markers, as a fraction of the cactus plots' end markers.
POINT_SCALE = 0.8
STYLE = dict(color="black", linewidth=1.2, marker="o", markersize=POINT_SCALE * marker_size("o"),
             markeredgewidth=POINT_SCALE * MARKER_EDGE_WIDTH, markeredgecolor="black", markerfacecolor="black")


def k_label(v: float, _pos: int) -> str:
    """A tick label in plain digits, in thousands from 1,000 up: 0.5, 20, 500, 5k."""
    return f"{v / 1000:g}k" if v >= 1000 else f"{v:g}"


def clause_counts() -> dict[tuple[str, int], int]:
    """(line, n) -> the instance's clause count."""
    with open(HERE / "instances.csv") as f:
        return {(f"{r['family']}_k{r['k']}", int(r["n"])): int(r["clauses"]) for r in csv.DictReader(f)}


def largest_solved() -> str:
    best: dict[tuple[str, str], int] = collections.defaultdict(int)
    for arm, _ in TABLE_ARMS:
        for r in measure.read_jsonl(RESULTS / f"{arm}.jsonl"):
            if r["verdict"] in ("SAT", "UNSAT"):
                best[(r["line"], arm)] = max(best[(r["line"], arm)], r["n"])
    with open(HERE / "lines.csv") as f:
        lines = [r["line"] for r in csv.DictReader(f)]
    out = ["Largest n solved within 300 s.", "", "| line | " + " | ".join(label for _, label in TABLE_ARMS) + " |",
           "|---|" + "---:|" * len(TABLE_ARMS)]
    out += [f"| {line} | " + " | ".join(str(best.get((line, a), "—")) for a, _ in TABLE_ARMS) + " |" for line in lines]
    return "\n".join(out) + "\n"


def fit(points: list[tuple[int, float]]) -> tuple[float, float] | None:
    """Exponent and intercept of a least-squares line through the points in log-log, or None under 3 points."""
    xy = [(math.log(n), math.log(y)) for n, y in points if y > 0]
    if len(xy) < 3:
        return None
    mx, my = st.mean(x for x, _ in xy), st.mean(y for _, y in xy)
    slope = sum((x - mx) * (y - my) for x, y in xy) / sum((x - mx) ** 2 for x, _ in xy)
    return slope, my - slope * mx


def main() -> int:
    (RESULTS / "largest_solved.md").write_text(largest_solved())
    out = RESULTS / "pc.png"
    timed = measure.read_jsonl(RESULTS / f"{ARM}.jsonl")
    proofs = measure.read_jsonl(RESULTS / "proofs" / f"{ARM}.jsonl")
    data = {"wall_s": [r for r in timed if r["verdict"] in ("SAT", "UNSAT")],
            "proof_lines": [r for r in proofs if r.get("check") == "verified"]}

    clauses = clause_counts()
    fig, axes = plt.subplots(len(ROWS), len(LINES), figsize=(9.6, 5.6), sharex="col", sharey="row")
    for col, (line, title) in enumerate(LINES.items()):
        lo = FIT_FROM[line]
        runs = sorted((r for r in timed if r["line"] == line), key=lambda r: r["n"])
        last_n = max(r["n"] for r in runs if r["verdict"] in ("SAT", "UNSAT"))
        last = clauses[(line, last_n)]
        after = [r for r in runs if r["n"] > last_n]
        memouts = [clauses[(line, r["n"])] for r in after] if after and all(r["mem_out"] for r in after) else []
        for row, (metric, ylabel, symbol) in enumerate(ROWS):
            ax = axes[row, col]
            pts = sorted((clauses[(line, r["n"])], r[metric]) for r in data[metric] if r["line"] == line and r["n"] >= lo)
            ax.plot([n for n, _ in pts], [y for _, y in pts], **{**STYLE, "linestyle": "none"})
            f = fit(pts)
            if f:
                xs = [pts[0][0], pts[-1][0]]
                ax.plot(xs, [math.exp(f[1]) * x ** f[0] for x in xs], **{**STYLE, "marker": None})
            ax.set_xscale("log")
            ax.set_yscale("log")
            if metric == "wall_s":
                ax.set_ylim(*TIME_LIM)
            if memouts:
                start = math.sqrt(last * memouts[0])
                end = memouts[-1] * 1.25
                ax.set_xlim(right=end)
                ax.axvspan(start, end, color="black", alpha=0.05, linewidth=0)
                ax.text(math.sqrt(start * end), 0.5, "MEMOUT", transform=ax.get_xaxis_transform(), rotation=90,
                        ha="center", va="center", fontsize=FONT_SIZE)
            if f:
                right = start if memouts else ax.get_xlim()[1]
                ax.annotate(rf"${symbol} \propto m$^{f[0]:.2f}", (right, 0), xycoords=ax.get_xaxis_transform(),
                            xytext=(-LAW_OFFSET[0], LAW_OFFSET[1]), textcoords="offset points", ha="right",
                            va="bottom", fontsize=FONT_SIZE)
            ax.tick_params(labelsize=FONT_SIZE)
            for axis, (a, b) in ((ax.xaxis, ax.get_xlim()), (ax.yaxis, ax.get_ylim())):
                subs = (1.0, 2.0, 5.0) if b / a >= 10 else (1.0, 1.5, 2.0, 3.0, 5.0, 7.0)
                axis.set_major_locator(LogLocator(base=10, subs=subs))
                axis.set_major_formatter(FuncFormatter(k_label))
                axis.set_minor_formatter(NullFormatter())
            if row == 0:
                ax.set_title(title, fontsize=FONT_SIZE)
            if row == len(ROWS) - 1:
                ax.set_xlabel(XLABEL, fontsize=FONT_SIZE)
            if col == 0:
                ax.set_ylabel(ylabel, fontsize=FONT_SIZE)
    fig.tight_layout()
    tight = fig.get_tightbbox(fig.canvas.get_renderer())
    fig.savefig(out, dpi=150, bbox_inches=Bbox.from_extents(tight.x0, 0, tight.x1, fig.get_figheight()))
    print(RESULTS / "largest_solved.md")
    print(out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
