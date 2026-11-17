#!/usr/bin/env python3
"""Write the cryptographic comparison's tables and figure from `../results/crypto/`.

    report.py

Each arm stands for its per-instance median over seeds 200, 201 and 202. An unsolved run
sorts above every solved one, so the median is solved exactly when two of the three seeds
solved. The third seed ran only where seed 200 or 201 solved, and a run missing there counts
as unsolved. Xorricane and 2-Xornado take no seed and have one run. PAR-2 charges an unsolved
run twice the timeout. Writes:

- `tables.md`: solved and PAR-2 per family and in total, and the unsolved runs by cause;
- `ablation.md`: the effect of each default-on heuristic (the paper's tab:heur);
- `overall.png`: cactus plots, all families pooled and one panel a family (the paper's fig:main).
"""

from __future__ import annotations

import math
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
import matplotlib.pyplot as plt
from matplotlib.lines import Line2D
from matplotlib.transforms import Bbox

import measure
from plot_style import FONT_SIZE, MARKER_EDGE_WIDTH, STYLES, marker_size

from run import RESULTS, load_instances

SEEDS = (200, 201, 202)
MAIN = [("Kissat 4.0.4", "kissat-4.0.4"), ("CaDiCaL 3.0.1", "cadical-3.0.1"), ("CMS 5.17.0", "cms-5.17.0"),
        ("CMS 5.13.0", "cms-5.13.0"), ("Xorricane 0.4.2.11", "xorricane-0.4.2.11"),
        ("2-Xornado 1.0.2", "2xornado-1.0.2"), ("Xorcle 0.1", "xorcle-0.1"), ("Xorcle 0.2", "xorcle-0.2")]
BASE = "xorcle-0.2"
ABLATION = {"selection=cyclic": "Replacing exact watchlist with cyclic selector",
            "deletion=false": "No clause deletion", "csids-intersection-depth=1": "No CSIDS intersection",
            "phase-saving=none": "No phase saving", "implication-graph=false": "No implication graph",
            "compact=false": "No compaction", "minimize=false": "No clause minimization",
            "vivify=false": "No vivification", "otf-subsume=false": "No OTF subsumption", "bve=false": "No BVE"}
#: The time ratio's instances: either configuration takes more than this many seconds, and either solves.
SLOW_S = 10


def solved(r: dict) -> bool:
    return r["verdict"] in ("SAT", "UNSAT")


def charged(r: dict) -> float:
    return r["wall_s"] if solved(r) else 2 * r["timeout_s"]


def medians(arm: str) -> dict[tuple[str, str], dict]:
    """Each instance's median run, as in the module docstring."""
    recs = measure.read_jsonl(RESULTS / f"{arm}.jsonl")
    if not measure.takes_seed(measure.load_arms()[arm]):
        return {(r["family"], r["name"]): r for r in recs}
    by_seed: dict[int, dict] = {s: {} for s in SEEDS}
    for r in recs:
        by_seed[r["order_seed"]][(r["family"], r["name"])] = r
    out = {}
    for k, first in by_seed[SEEDS[0]].items():
        runs = [first] + [by_seed[s][k] for s in SEEDS[1:] if k in by_seed[s]]
        if len(runs) < len(SEEDS) and any(solved(r) for r in runs):
            raise SystemExit(f"{arm} {k}: a seed is missing a run although one seed solved")
        runs.sort(key=lambda r: (not solved(r), r["wall_s"]))
        out[k] = runs[(len(SEEDS) - 1) // 2] if len(runs) == len(SEEDS) else runs[-1]
    return out


def tables(instances: list[dict], data: dict[str, dict]) -> str:
    families = sorted({r["family"] for r in instances})
    n = {f: sum(r["family"] == f for r in instances) for f in families}
    out = ["Solved · PAR-2 (thousands of seconds), per-instance medians over seeds 200-202.", "",
           "| family | n | " + " | ".join(label for label, _ in MAIN) + " |", "|---|---:|" + "---:|" * len(MAIN)]
    totals = {arm: [0, 0.0] for _, arm in MAIN}
    for f in families:
        cells = []
        for _, arm in MAIN:
            rs = [r for (fam, _), r in data[arm].items() if fam == f]
            s, p = sum(map(solved, rs)), sum(map(charged, rs))
            totals[arm][0] += s
            totals[arm][1] += p
            cells.append(f"{s} · {p / 1000:.1f}")
        out.append(f"| {f} | {n[f]} | " + " | ".join(cells) + " |")
    out.append(f"| **total** | {len(instances)} | "
               + " | ".join(f"{s} · {p / 1000:.1f}" for s, p in totals.values()) + " |")
    by_instance: dict[tuple, set] = {}
    for _, arm in MAIN:
        for k, r in data[arm].items():
            if solved(r):
                by_instance.setdefault(k, set()).add(r["verdict"])
    out += ["", f"Solved by one of these arms: {len(by_instance)} of {len(instances)}; verdicts "
            f"{sorted({v for s in by_instance.values() for v in s})}; instances on which arms disagree: "
            f"{sum(len(s) > 1 for s in by_instance.values())}.", "", "Unsolved runs by cause:", "",
            "| arm | timeout | memory-out | other |", "|---|---:|---:|---:|"]
    for label, arm in MAIN:
        rs = [r for r in data[arm].values() if not solved(r)]
        t, m = sum(r["timed_out"] for r in rs), sum(r["mem_out"] for r in rs)
        out.append(f"| {label} | {t} | {m} | {len(rs) - t - m} |")
    return "\n".join(out) + "\n"


def ablation(n: int, base: dict, data: dict[str, dict]) -> str:
    base_solved = {k for k, r in base.items() if solved(r)}
    rows = []
    for flag, label in ABLATION.items():
        recs = data[f"{BASE}_{flag}"]
        s = {k for k, r in recs.items() if solved(r)}
        logs = [math.log(charged(recs[k]) / charged(base[k])) for k in base
                if max(charged(base[k]), charged(recs[k])) > SLOW_S and (solved(base[k]) or solved(recs[k]))]
        par2 = sum(map(charged, recs.values())) / n
        rows.append((len(s), -par2, label, f"+{len(s - base_solved)} / −{len(base_solved - s)}", par2,
                     math.exp(sum(logs) / len(logs)), len(logs)))
    rows.sort()
    out = [f"| configuration | solved (of {n}) | gained / lost | average PAR-2 | time ratio | time ratio's instances |",
           "|---|---:|---:|---:|---:|---:|",
           f"| Xorcle default | {len(base_solved)} | — | {sum(map(charged, base.values())) / n:.1f} s | — | — |"]
    out += [f"| {label} | {s} | {gl} | {p:.1f} s | {g:.2f}× | {k} |" for s, _, label, gl, p, g, k in rows]
    return "\n".join(out) + "\n"


def draw(ax, label: str, times: list[float], linewidth: float) -> Line2D:
    """One cactus curve with a single marker on its last point; returns its legend handle."""
    color, style, marker, filled = STYLES[label]
    mark = dict(marker=marker, markersize=marker_size(marker), markeredgecolor=color,
                markerfacecolor=color if filled else "white", markeredgewidth=MARKER_EDGE_WIDTH)
    if times:
        ax.plot(range(1, len(times) + 1), times, color=color, linestyle=style, linewidth=linewidth)
        ax.plot([len(times)], [times[-1]], linestyle="none", clip_on=False, zorder=3, **mark)
    return Line2D([], [], color=color, linestyle=style, linewidth=linewidth, **mark)


#: Legend names where the tables abbreviate.
NAMES = {"CMS 5.17.0": "CryptoMiniSat 5.17.0", "CMS 5.13.0": "CryptoMiniSat 5.13.0"}
#: Legend entries, longest first; the figure takes the first whose legend fits its panel.
LEGEND_FORMATS = ("{name} ({solved}/{total} solved)", "{name} ({solved} solved)", "{name} ({solved}/{total})")
#: Panel titles; rand_qp's types are the Fukuoka MQ challenge's names.
TITLES = {"rand_qp_type_I": "rand_qp Type I", "rand_qp_type_IV": "rand_qp Type IV"}


def cactus(instances: list[dict], data: dict[str, dict], out: Path) -> None:
    families = sorted({r["family"] for r in instances})
    n = {f: sum(r["family"] == f for r in instances) for f in families}
    panels = [("Overall", None, len(instances))] + [(TITLES.get(f, f), f, n[f]) for f in families]
    ncols = 3
    nrows = -(-(len(panels) + 1) // ncols)
    fig, axes = plt.subplots(nrows, ncols, figsize=(3.2 * ncols, 2.6 * nrows), sharey=True)
    axes = axes.ravel()
    handles = {}
    for ax, (title, f, count) in zip(axes, panels):
        for label, arm in MAIN:
            times = sorted(r["wall_s"] for (fam, _), r in data[arm].items() if (f is None or fam == f) and solved(r))
            handles[label] = draw(ax, label, times, 1.2)
        ax.set_title(title, fontsize=FONT_SIZE)
        ax.set_xlim(0, count)
        ax.set_ylim(0, 300)
        ax.tick_params(labelsize=FONT_SIZE)
    for ax in axes[len(panels) - ncols:len(panels)]:
        ax.set_xlabel("instances solved", fontsize=FONT_SIZE)
    for ax in axes[::ncols]:
        ax.set_ylabel("median time (s)", fontsize=FONT_SIZE)
    for ax in axes[len(panels):]:
        ax.axis("off")
    count = {label: sum(map(solved, data[arm].values())) for label, arm in MAIN}
    renderer = fig.canvas.get_renderer()
    for fmt in LEGEND_FORMATS:
        texts = [fmt.format(name=NAMES.get(label, label), solved=count[label], total=len(instances)) for label in handles]
        legend = axes[-1].legend(handles.values(), texts, loc="center", frameon=False, fontsize=FONT_SIZE)
        fig.tight_layout()
        width = legend.get_window_extent(renderer).width / axes[-1].get_window_extent(renderer).width
        if width <= 1 or fmt == LEGEND_FORMATS[-1]:
            break
        legend.remove()
    # No margin left or right: crop to the drawn content horizontally, keep the full height.
    tight = fig.get_tightbbox(fig.canvas.get_renderer())
    fig.savefig(out, dpi=150, bbox_inches=Bbox.from_extents(tight.x0, 0, tight.x1, fig.get_figheight()))


def main() -> int:
    instances = load_instances()
    arms = [arm for _, arm in MAIN] + [f"{BASE}_{flag}" for flag in ABLATION]
    data = {arm: medians(arm) for arm in arms}
    (RESULTS / "tables.md").write_text(tables(instances, data))
    (RESULTS / "ablation.md").write_text(ablation(len(instances), data[BASE], data))
    cactus(instances, data, RESULTS / "overall.png")
    for name in ("tables.md", "ablation.md", "overall.png"):
        print(RESULTS / name)
    return 0


if __name__ == "__main__":
    sys.exit(main())
