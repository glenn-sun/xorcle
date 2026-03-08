#!/usr/bin/env python3
from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
import csv
import os
import re
import sys
import time
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path
from types import SimpleNamespace
from typing import Dict, List, Optional, Sequence

os.environ.setdefault("MPLCONFIGDIR", str(Path("/tmp") / "parity-sat-cpp-mplconfig"))

sys.path.append(str(Path(__file__).resolve().parent))
import run_benchmark as bs
from benchmark_plot_style import (
    apply_common_axes_style,
    create_unified_figure,
    finalize_figure,
    place_right_legend,
    series_style,
)
from benchmark_orchestration import (
    ALL_SOLVER_LINES,
    SOLVER_COLORS,
    SOLVER_DISPLAY,
    decisions_for_row,
    format_progress_line,
    median_is_timeout,
    runtime_for_row,
    should_report_progress,
    solver_line_key,
    timeout_aware_median_decisions,
    timeout_aware_median_runtime,
)


RANDOM_NAME_RE = re.compile(r"^.+_(\d+)_(\d{2})$")
DETERMINISTIC_NAME_RE = re.compile(r"^.+_(\d+)$")


@dataclass(frozen=True)
class FamilyRunSpec:
    name: str
    input_format: str  # cnf or xnf
    kind: str  # random or deterministic
    x_label: str
    x_values: List[int]
    files_by_x: Dict[int, List[Path]]


def repo_root() -> Path:
    return Path(__file__).resolve().parents[1]


def parse_args() -> argparse.Namespace:
    root = repo_root()
    parser = argparse.ArgumentParser(description="Type 1 benchmark: all families, all solvers, timeout-aware median trendline.")
    parser.add_argument("--tests-root", default=str(root / "tests"))
    parser.add_argument("--output-root", default=str(root / "benchmarks" / "type1"))
    parser.add_argument("--solver-bin", default=str(root / "bin" / "main-release"))
    parser.add_argument("--2xornado-bin", dest="xornado_bin", default=str(root / "external" / "2xornado"))
    parser.add_argument("--xorricane-bin", default=str(root / "external" / "xorricane"))
    parser.add_argument("--kissat-bin", default=str(root / "external" / "kissat"))
    parser.add_argument("--cms-bin", default=str(root / "external" / "cryptominisat5"))
    parser.add_argument("--attempts", type=int, default=1)
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--timeout", type=float, default=5.0)
    parser.add_argument("--saveconverted", action="store_true")
    parser.add_argument("--full", action="store_true", help="Shorthand for --attempts 5 --timeout 60")
    parser.add_argument("--families", nargs="*", default=None, help="Family folder names (or paths) to benchmark")
    parser.add_argument(
        "--solvers",
        nargs="*",
        choices=list(ALL_SOLVER_LINES),
        default=None,
        help="Optional solver subset to benchmark/replot. Default: all type-1 solver lines.",
    )
    parser.add_argument("--replot-only", action="store_true", help="Regenerate plots from existing CSV files only.")
    args = parser.parse_args()

    if args.full:
        args.attempts = 5
        args.timeout = 60.0

    if args.attempts < 1:
        parser.error("--attempts must be >= 1")
    if args.jobs < 1:
        parser.error("--jobs must be >= 1")
    if args.timeout <= 0:
        parser.error("--timeout must be > 0")
    return args


def infer_x_label(family_name: str) -> str:
    if family_name.endswith("_n") or "_n_" in family_name:
        return "n"
    if family_name.endswith("_h") or "_h_" in family_name:
        return "h"
    if family_name.endswith("_k") or "_k_" in family_name or "kxnf" in family_name or "klift" in family_name:
        return "k"
    return "x"


def sniff_input_format(sample_file: Path, family_name: str) -> str:
    try:
        with sample_file.open("r", encoding="utf-8", errors="replace") as f:
            for raw in f:
                line = raw.strip()
                if not line or line.startswith("c") or line.startswith("p"):
                    continue
                if "+" in line or line.startswith("x"):
                    return "xnf"
                break
    except OSError:
        pass
    lower = family_name.lower()
    if lower.startswith("tseitin") or lower.startswith("bijphp"):
        return "cnf"
    return "xnf"


def discover_family_spec(family_dir: Path) -> FamilyRunSpec:
    files = sorted(p for p in family_dir.glob("*.cnf") if p.is_file())
    if not files:
        raise RuntimeError(f"{family_dir.name}: no .cnf files found in family directory")

    files_by_x: Dict[int, List[Path]] = defaultdict(list)
    kinds_seen: set[str] = set()
    for p in files:
        stem = p.stem
        m_random = RANDOM_NAME_RE.match(stem)
        if m_random:
            kinds_seen.add("random")
            x = int(m_random.group(1))
            files_by_x[x].append(p)
            continue
        m_det = DETERMINISTIC_NAME_RE.match(stem)
        if m_det:
            kinds_seen.add("deterministic")
            x = int(m_det.group(1))
            files_by_x[x].append(p)
            continue

    if not files_by_x:
        raise RuntimeError(
            f"{family_dir.name}: could not parse size parameter from filenames; expected '*_<x>.cnf' "
            "or '*_<x>_<attempt:02d>.cnf'"
        )
    if len(kinds_seen) != 1:
        raise RuntimeError(f"{family_dir.name}: mixed random/deterministic filename patterns in one family")

    kind = next(iter(kinds_seen))
    x_values = sorted(files_by_x.keys())
    input_format = sniff_input_format(files[0], family_dir.name)
    return FamilyRunSpec(
        name=family_dir.name,
        input_format=input_format,
        kind=kind,
        x_label=infer_x_label(family_dir.name),
        x_values=x_values,
        files_by_x={x: sorted(v) for x, v in files_by_x.items()},
    )


def resolve_family_dirs(*, tests_root: Path, family_args: Sequence[str] | None) -> List[Path]:
    if not family_args:
        dirs = []
        for child in sorted(tests_root.iterdir()):
            if not child.is_dir():
                continue
            if any(child.glob("*.cnf")):
                dirs.append(child)
        if not dirs:
            raise RuntimeError(f"No benchmark family directories with .cnf files found under: {tests_root}")
        return dirs

    resolved: List[Path] = []
    for item in family_args:
        candidate = Path(item)
        if candidate.is_absolute():
            family_dir = candidate
        else:
            family_dir = tests_root / candidate
            if not family_dir.is_dir():
                family_dir = candidate
        if not family_dir.is_dir():
            raise FileNotFoundError(f"Missing family directory: {item}")
        resolved.append(family_dir.resolve())
    return resolved


def make_runner_args(
    *,
    cli_args: argparse.Namespace,
    input_format: str,
    repeat: int,
    solvers: Sequence[str],
) -> SimpleNamespace:
    return SimpleNamespace(
        input_format=input_format,
        repeat=repeat,
        timeout=cli_args.timeout,
        solver_bin=str(Path(cli_args.solver_bin).resolve()),
        kissat_bin=str(Path(cli_args.kissat_bin).resolve()),
        cms_bin=str(Path(cli_args.cms_bin).resolve()),
        xornado_bin=str(Path(cli_args.xornado_bin).resolve()),
        xorricane_bin=str(Path(cli_args.xorricane_bin).resolve()),
        csv_path="",
        kissat_mode="both",
        exp_cnf=False,
        keep_going=True,
        jobs=1,
        solvers=list(solvers),
        saveconverted=bool(cli_args.saveconverted),
    )


def selected_solver_lines(cli_args: argparse.Namespace) -> List[str]:
    return list(cli_args.solvers) if cli_args.solvers else list(ALL_SOLVER_LINES)


def write_csv(path: Path, rows: Sequence[Dict[str, str]], fieldnames: Sequence[str]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fieldnames)
        w.writeheader()
        w.writerows(rows)


def read_csv_rows(path: Path) -> List[Dict[str, str]]:
    with path.open("r", encoding="utf-8", newline="") as f:
        return list(csv.DictReader(f))


def family_name_from_arg(raw: str) -> str:
    return Path(raw).name or Path(raw).resolve().name


def agg_csv_family_name(path: Path) -> str:
    suffix = ".agg.csv"
    return path.name[: -len(suffix)] if path.name.endswith(suffix) else path.stem


def solver_plot_style(solver: str, index: int):
    return series_style(index, color_override=SOLVER_COLORS.get(solver))


def draw_segmented_series(
    ax,
    *,
    x_values: Sequence[int],
    median_by_x: Dict[int, float | None],
    label: str,
    color: str,
    linestyle,
    linewidth: float = 1.3,
) -> None:
    current_x: List[int] = []
    current_y: List[float] = []
    drew_label = False
    for x in x_values:
        value = median_by_x.get(x)
        if value is None:
            if current_x:
                ax.plot(
                    current_x,
                    current_y,
                    color=color,
                    linestyle=linestyle,
                    linewidth=linewidth,
                    label=(label if not drew_label else "_nolegend_"),
                )
                drew_label = True
                current_x = []
                current_y = []
            continue
        current_x.append(x)
        current_y.append(value)
    if current_x:
        ax.plot(
            current_x,
            current_y,
            color=color,
            linestyle=linestyle,
            linewidth=linewidth,
            label=(label if not drew_label else "_nolegend_"),
        )
        drew_label = True
    if not drew_label:
        ax.plot([], [], color=color, linestyle=linestyle, linewidth=linewidth, label=label)


def load_plot_inputs_from_agg(
    path: Path,
) -> tuple[
    str,
    List[int],
    str,
    Dict[str, List[tuple[int, float]]],
    Dict[str, Dict[int, float | None]],
    Dict[str, List[tuple[int, float]]],
    Dict[str, Dict[int, float | None]],
]:
    rows = read_csv_rows(path)
    if not rows:
        raise RuntimeError(f"{path.name}: no rows found")

    family = rows[0].get("family", "") or agg_csv_family_name(path)
    x_label = rows[0].get("x_label", "") or infer_x_label(family)
    x_values = sorted({int(row["x_value"]) for row in rows if row.get("x_value", "").strip()})

    points_by_solver: Dict[str, List[tuple[int, float]]] = defaultdict(list)
    medians_by_solver: Dict[str, Dict[int, float | None]] = defaultdict(dict)
    decision_points_by_solver: Dict[str, List[tuple[int, float]]] = defaultdict(list)
    decision_medians_by_solver: Dict[str, Dict[int, float | None]] = defaultdict(dict)

    for row in rows:
        solver = row.get("solver", "").strip()
        if solver not in ALL_SOLVER_LINES:
            continue
        x_raw = row.get("x_value", "").strip()
        if not x_raw:
            continue
        x = int(x_raw)
        series = row.get("series", "").strip()
        time_raw = row.get("time_s", "").strip()
        decisions_raw = row.get("decisions", "").strip()

        if series == "point":
            if time_raw:
                points_by_solver[solver].append((x, float(time_raw)))
            if decisions_raw:
                decisions = float(decisions_raw)
                if decisions > 0.0:
                    decision_points_by_solver[solver].append((x, decisions))
            continue

        if series == "median":
            medians_by_solver[solver][x] = float(time_raw) if time_raw else None
            if decisions_raw:
                decisions = float(decisions_raw)
                decision_medians_by_solver[solver][x] = decisions if decisions > 0.0 else None
            else:
                decision_medians_by_solver[solver][x] = None

    return (
        family,
        x_values,
        x_label,
        points_by_solver,
        medians_by_solver,
        decision_points_by_solver,
        decision_medians_by_solver,
    )


def plot_family(
    *,
    path: Path,
    family: str,
    x_values: Sequence[int],
    x_label: str,
    solver_lines: Sequence[str],
    points_by_solver: Dict[str, List[tuple[int, float]]],
    medians_by_solver: Dict[str, Dict[int, float | None]],
) -> None:
    try:
        import matplotlib

        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ModuleNotFoundError as exc:  # pragma: no cover
        raise SystemExit(f"Error: missing dependency '{exc.name}'. Install matplotlib to run plotting.")

    path.parent.mkdir(parents=True, exist_ok=True)
    fig, ax = create_unified_figure(plt)

    for idx, solver in enumerate(solver_lines):
        style = solver_plot_style(solver, idx)
        label = SOLVER_DISPLAY[solver]
        pts = sorted(points_by_solver.get(solver, []), key=lambda p: (p[0], p[1]))
        if pts:
            ax.scatter(
                [x for x, _ in pts],
                [y for _, y in pts],
                s=14,
                alpha=0.70,
                color=style.color,
                marker=style.marker,
                label="_nolegend_",
            )

        draw_segmented_series(
            ax,
            x_values=x_values,
            median_by_x=medians_by_solver.get(solver, {}),
            label=label,
            color=style.color,
            linestyle=style.linestyle,
        )

    ax.set_yscale("log")
    apply_common_axes_style(ax, xlabel=x_label, ylabel="Time (s)", title=family)
    place_right_legend(ax)
    finalize_figure(fig, path)
    plt.close(fig)


def plot_family_decisions(
    *,
    path: Path,
    family: str,
    x_values: Sequence[int],
    x_label: str,
    solver_lines: Sequence[str],
    points_by_solver: Dict[str, List[tuple[int, float]]],
    medians_by_solver: Dict[str, Dict[int, float | None]],
) -> None:
    try:
        import matplotlib

        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ModuleNotFoundError as exc:  # pragma: no cover
        raise SystemExit(f"Error: missing dependency '{exc.name}'. Install matplotlib to run plotting.")

    path.parent.mkdir(parents=True, exist_ok=True)
    fig, ax = create_unified_figure(plt)

    for idx, solver in enumerate(solver_lines):
        style = solver_plot_style(solver, idx)
        label = SOLVER_DISPLAY[solver]
        # Log scale requires strictly positive values.
        pts = sorted((x, y) for x, y in points_by_solver.get(solver, []) if y > 0.0)
        if pts:
            ax.scatter(
                [x for x, _ in pts],
                [y for _, y in pts],
                s=14,
                alpha=0.70,
                color=style.color,
                marker=style.marker,
                label="_nolegend_",
            )

        draw_segmented_series(
            ax,
            x_values=x_values,
            median_by_x={x: y for x, y in medians_by_solver.get(solver, {}).items() if y is not None and y > 0.0},
            label=label,
            color=style.color,
            linestyle=style.linestyle,
        )

    ax.set_yscale("log")
    apply_common_axes_style(ax, xlabel=x_label, ylabel="Decisions", title=f"{family} (Decisions)")
    place_right_legend(ax)
    finalize_figure(fig, path)
    plt.close(fig)


def run_size_task(
    *,
    cli_args: argparse.Namespace,
    input_format: str,
    solver: str,
    file_path: Path,
    converter_script: Path,
    run_index_override: int | None = None,
) -> List[Dict[str, str]]:
    runner_args = make_runner_args(
        cli_args=cli_args,
        input_format=input_format,
        repeat=1,
        solvers=[solver],
    )
    rows = bs.benchmark_file(file_path, runner_args, [], converter_script)
    if run_index_override is not None:
        run_idx_str = str(run_index_override)
        for row in rows:
            row["run_index"] = run_idx_str
    return rows


def replot_from_existing_csv(*, cli_args: argparse.Namespace, output_root: Path) -> int:
    csv_root = output_root / "csv"
    plots_root = output_root / "plots"
    plots_root.mkdir(parents=True, exist_ok=True)
    solver_lines = selected_solver_lines(cli_args)

    if cli_args.families:
        family_names = [family_name_from_arg(item) for item in cli_args.families]
    else:
        family_names = sorted(agg_csv_family_name(path) for path in csv_root.glob("*.agg.csv"))

    if not family_names:
        print(f"[type1] no aggregate CSV files found under {csv_root}; skipping")
        return 0

    plotted = 0
    for family in family_names:
        agg_csv_path = csv_root / f"{family}.agg.csv"
        if not agg_csv_path.is_file():
            print(f"[type1] skipping {family}: missing CSV {agg_csv_path}")
            continue

        (
            csv_family,
            x_values,
            x_label,
            points_by_solver,
            medians_by_solver,
            decision_points_by_solver,
            decision_medians_by_solver,
        ) = load_plot_inputs_from_agg(agg_csv_path)

        plot_path = plots_root / f"{csv_family}.png"
        plot_family(
            path=plot_path,
            family=csv_family,
            x_values=x_values,
            x_label=x_label,
            solver_lines=solver_lines,
            points_by_solver=points_by_solver,
            medians_by_solver=medians_by_solver,
        )

        decision_plot_path = plots_root / f"{csv_family}_decisions.png"
        plot_family_decisions(
            path=decision_plot_path,
            family=csv_family,
            x_values=x_values,
            x_label=x_label,
            solver_lines=solver_lines,
            points_by_solver=decision_points_by_solver,
            medians_by_solver=decision_medians_by_solver,
        )

        plotted += 1
        print(f"[type1] replot wrote: {plot_path}")
        print(f"[type1] replot wrote: {decision_plot_path}")

    if plotted == 0:
        print(f"[type1] no plots regenerated from {csv_root}")
    return 0


def main() -> int:
    args = parse_args()
    root = repo_root()
    tests_root = Path(args.tests_root).resolve()
    output_root = Path(args.output_root).resolve()
    csv_root = output_root / "csv"
    plots_root = output_root / "plots"
    converter_script = root / "tools" / "xnf_convert.py"

    csv_root.mkdir(parents=True, exist_ok=True)
    plots_root.mkdir(parents=True, exist_ok=True)
    solver_lines = selected_solver_lines(args)

    if args.replot_only:
        return replot_from_existing_csv(cli_args=args, output_root=output_root)

    family_dirs = resolve_family_dirs(tests_root=tests_root, family_args=args.families)
    family_specs = [discover_family_spec(d) for d in family_dirs]
    total_chunks = sum(len(spec.x_values) for spec in family_specs)
    done_chunks = 0
    start_all = time.perf_counter()

    for spec in family_specs:
        family = spec.name
        files_by_x = spec.files_by_x

        # Validate inventory against attempts semantics.
        for x in spec.x_values:
            count = len(files_by_x[x])
            if spec.kind == "random" and count < args.attempts:
                raise RuntimeError(
                    f"{family}: need at least {args.attempts} random instances for x={x}, found {count}. "
                    "Regenerate tests with tools/generate_xnf_tests.py --attempts ..."
                )
            if spec.kind == "deterministic" and count < 1:
                raise RuntimeError(f"{family}: missing deterministic instance for x={x}")

        family_raw_rows: List[Dict[str, str]] = []
        solver_x_rows: Dict[tuple[str, int], List[Dict[str, str]]] = defaultdict(list)
        point_metrics_by_solver: Dict[str, List[tuple[int, Optional[float], Optional[float]]]] = defaultdict(list)
        points_by_solver: Dict[str, List[tuple[int, float]]] = defaultdict(list)
        medians_by_solver: Dict[str, Dict[int, float | None]] = defaultdict(dict)
        decision_points_by_solver: Dict[str, List[tuple[int, float]]] = defaultdict(list)
        decision_medians_by_solver: Dict[str, Dict[int, float | None]] = defaultdict(dict)

        active_solvers = list(solver_lines)
        timeout_streak = {s: 0 for s in solver_lines}

        for x in spec.x_values:
            selected_files = files_by_x[x][: args.attempts] if spec.kind == "random" else [files_by_x[x][0]]

            chunk_rows: List[Dict[str, str]] = []
            if active_solvers:
                with ThreadPoolExecutor(max_workers=args.jobs) as executor:
                    futures = []
                    if spec.kind == "random":
                        for file_path in selected_files:
                            for solver in active_solvers:
                                futures.append(
                                    executor.submit(
                                        run_size_task,
                                        cli_args=args,
                                        input_format=spec.input_format,
                                        solver=solver,
                                        file_path=file_path,
                                        converter_script=converter_script,
                                    )
                                )
                    else:
                        file_path = selected_files[0]
                        for run_idx in range(1, args.attempts + 1):
                            for solver in active_solvers:
                                futures.append(
                                    executor.submit(
                                        run_size_task,
                                        cli_args=args,
                                        input_format=spec.input_format,
                                        solver=solver,
                                        file_path=file_path,
                                        converter_script=converter_script,
                                        run_index_override=run_idx,
                                    )
                                )
                    for future in as_completed(futures):
                        chunk_rows.extend(future.result())

            family_raw_rows.extend(chunk_rows)
            for row in chunk_rows:
                solver = solver_line_key(row)
                if solver is None:
                    continue
                solver_x_rows[(solver, x)].append(row)
                t = runtime_for_row(row)
                d = decisions_for_row(row)
                if t is not None or d is not None:
                    point_metrics_by_solver[solver].append((x, t, d))
                if t is not None:
                    points_by_solver[solver].append((x, t))
                if d is not None:
                    decision_points_by_solver[solver].append((x, d))

            still_active: List[str] = []
            for solver in active_solvers:
                rows_for_solver = solver_x_rows.get((solver, x), [])
                med = timeout_aware_median_runtime(rows_for_solver)
                medians_by_solver[solver][x] = med
                decision_medians_by_solver[solver][x] = timeout_aware_median_decisions(rows_for_solver)

                if median_is_timeout(rows_for_solver):
                    timeout_streak[solver] += 1
                else:
                    timeout_streak[solver] = 0

                if timeout_streak[solver] < 2:
                    still_active.append(solver)

            dropped = [s for s in active_solvers if s not in still_active]
            if dropped:
                print(f"[{family}] x={x}: stopping {dropped} after 2 consecutive median-timeout sizes")
            active_solvers = still_active

            done_chunks += 1
            if should_report_progress(done_chunks, total_chunks, every=1):
                print(
                    format_progress_line(
                        done=done_chunks,
                        total=total_chunks,
                        start_s=start_all,
                        family=family,
                        x=x,
                    )
                )

        if not family_raw_rows:
            raise RuntimeError(f"{family}: no benchmark rows produced")

        family_raw_rows.sort(
            key=lambda row: (
                Path(row.get("file", "")).name,
                int(row.get("run_index", "0") or 0),
                row.get("solver_config", ""),
                row.get("backend_input_kind", ""),
                row.get("solver", ""),
            )
        )

        raw_csv_path = csv_root / f"{family}.raw.csv"
        write_csv(raw_csv_path, family_raw_rows, bs.RAW_CSV_FIELDS)

        agg_rows: List[Dict[str, str]] = []
        for solver in solver_lines:
            for x, t, d in sorted(
                point_metrics_by_solver.get(solver, []),
                key=lambda p: (p[0], float("inf") if p[1] is None else p[1]),
            ):
                agg_rows.append(
                    {
                        "family": family,
                        "x_label": spec.x_label,
                        "x_value": str(x),
                        "solver": solver,
                        "series": "point",
                        "time_s": "" if t is None else f"{t:.6f}",
                        "decisions": "" if d is None else str(int(d)),
                        "samples": "1",
                        "status": "",
                    }
                )
            for x in spec.x_values:
                rows_for_solver = solver_x_rows.get((solver, x), [])
                med = medians_by_solver.get(solver, {}).get(x)
                decisions_med = decision_medians_by_solver.get(solver, {}).get(x)
                agg_rows.append(
                    {
                        "family": family,
                        "x_label": spec.x_label,
                        "x_value": str(x),
                        "solver": solver,
                        "series": "median",
                        "time_s": "" if med is None else f"{med:.6f}",
                        "decisions": "" if decisions_med is None else str(int(decisions_med)),
                        "samples": str(len(rows_for_solver)),
                        "status": "TIMEOUT" if med is None and rows_for_solver else "",
                    }
                )

        agg_csv_path = csv_root / f"{family}.agg.csv"
        write_csv(
            agg_csv_path,
            agg_rows,
            ["family", "x_label", "x_value", "solver", "series", "time_s", "decisions", "samples", "status"],
        )

        plot_path = plots_root / f"{family}.png"
        plot_family(
            path=plot_path,
            family=family,
            x_values=spec.x_values,
            x_label=spec.x_label,
            solver_lines=solver_lines,
            points_by_solver=points_by_solver,
            medians_by_solver=medians_by_solver,
        )
        decision_plot_path = plots_root / f"{family}_decisions.png"
        plot_family_decisions(
            path=decision_plot_path,
            family=family,
            x_values=spec.x_values,
            x_label=spec.x_label,
            solver_lines=solver_lines,
            points_by_solver=decision_points_by_solver,
            medians_by_solver=decision_medians_by_solver,
        )

        print(f"[{family}] wrote: {raw_csv_path}")
        print(f"[{family}] wrote: {agg_csv_path}")
        print(f"[{family}] wrote: {plot_path}")
        print(f"[{family}] wrote: {decision_plot_path}")

    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (FileNotFoundError, RuntimeError, ValueError) as exc:
        print(f"Error: {exc}", file=sys.stderr)
        raise SystemExit(2)
