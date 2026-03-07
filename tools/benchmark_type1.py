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


def write_csv(path: Path, rows: Sequence[Dict[str, str]], fieldnames: Sequence[str]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fieldnames)
        w.writeheader()
        w.writerows(rows)


def plot_family(
    *,
    path: Path,
    family: str,
    x_values: Sequence[int],
    x_label: str,
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
    fig, ax = plt.subplots(figsize=(4.6, 3.1))

    for solver in ALL_SOLVER_LINES:
        color = SOLVER_COLORS[solver]
        label = SOLVER_DISPLAY[solver]
        pts = sorted(points_by_solver.get(solver, []), key=lambda p: (p[0], p[1]))
        if pts:
            ax.scatter([x for x, _ in pts], [y for _, y in pts], s=12, alpha=0.70, color=color, label="_nolegend_")

        current_x: List[int] = []
        current_y: List[float] = []
        drew_label = False
        for x in x_values:
            m = medians_by_solver.get(solver, {}).get(x)
            if m is None:
                if current_x:
                    ax.plot(current_x, current_y, color=color, linewidth=1.2, label=(label if not drew_label else "_nolegend_"))
                    drew_label = True
                    current_x = []
                    current_y = []
                continue
            current_x.append(x)
            current_y.append(m)
        if current_x:
            ax.plot(current_x, current_y, color=color, linewidth=1.2, label=(label if not drew_label else "_nolegend_"))
            drew_label = True
        if not drew_label:
            ax.plot([], [], color=color, linewidth=1.2, label=label)

    ax.set_xlabel(x_label)
    ax.set_ylabel("Time (s)")
    ax.set_title(family)
    ax.set_yscale("log")
    ax.grid(True, linestyle="--", linewidth=0.5, alpha=0.35)
    ax.legend(frameon=False, fontsize=8)
    fig.tight_layout(pad=0.5)
    fig.savefig(path, dpi=300)
    plt.close(fig)


def plot_family_decisions(
    *,
    path: Path,
    family: str,
    x_values: Sequence[int],
    x_label: str,
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
    fig, ax = plt.subplots(figsize=(4.6, 3.1))

    for solver in ALL_SOLVER_LINES:
        color = SOLVER_COLORS[solver]
        label = SOLVER_DISPLAY[solver]
        # Log scale requires strictly positive values.
        pts = sorted((x, y) for x, y in points_by_solver.get(solver, []) if y > 0.0)
        if pts:
            ax.scatter([x for x, _ in pts], [y for _, y in pts], s=12, alpha=0.70, color=color, label="_nolegend_")

        current_x: List[int] = []
        current_y: List[float] = []
        drew_label = False
        for x in x_values:
            m = medians_by_solver.get(solver, {}).get(x)
            if m is None or m <= 0.0:
                if current_x:
                    ax.plot(current_x, current_y, color=color, linewidth=1.2, label=(label if not drew_label else "_nolegend_"))
                    drew_label = True
                    current_x = []
                    current_y = []
                continue
            current_x.append(x)
            current_y.append(m)
        if current_x:
            ax.plot(current_x, current_y, color=color, linewidth=1.2, label=(label if not drew_label else "_nolegend_"))
            drew_label = True
        if not drew_label:
            ax.plot([], [], color=color, linewidth=1.2, label=label)

    ax.set_xlabel(x_label)
    ax.set_ylabel("Decisions")
    ax.set_title(f"{family} (Decisions)")
    ax.set_yscale("log")
    ax.grid(True, linestyle="--", linewidth=0.5, alpha=0.35)
    ax.legend(frameon=False, fontsize=8)
    fig.tight_layout(pad=0.5)
    fig.savefig(path, dpi=300)
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

        active_solvers = list(ALL_SOLVER_LINES)
        timeout_streak = {s: 0 for s in ALL_SOLVER_LINES}

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
        for solver in ALL_SOLVER_LINES:
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
            points_by_solver=points_by_solver,
            medians_by_solver=medians_by_solver,
        )
        decision_plot_path = plots_root / f"{family}_decisions.png"
        plot_family_decisions(
            path=decision_plot_path,
            family=family,
            x_values=spec.x_values,
            x_label=spec.x_label,
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
