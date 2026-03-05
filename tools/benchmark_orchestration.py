#!/usr/bin/env python3
from __future__ import annotations

import math
import re
import statistics
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional, Pattern, Sequence

ALL_SOLVER_LINES = (
    "xorcle_0_1",
    "2xornado",
    "xorricane",
    "kissat_plain",
    "kissat_default",
    "cryptominisat",
)

SOLVER_TO_BENCH_ARG = {
    "xorcle_0_1": "xorcle_0_1",
    "2xornado": "2xornado",
    "xorricane": "xorricane",
    "kissat_plain": "kissat_plain",
    "kissat_default": "kissat_default",
    "cryptominisat": "cryptominisat",
}

SOLVER_DISPLAY = {
    "xorcle_0_1": "Xorcle 0.1",
    "2xornado": "2-Xornado",
    "xorricane": "Xorricane",
    "kissat_plain": "Kissat --plain",
    "kissat_default": "Kissat",
    "cryptominisat": "CryptoMiniSat",
}

SOLVER_COLORS = {
    "xorcle_0_1": "#1f77b4",
    "2xornado": "#9467bd",
    "xorricane": "#8c564b",
    "kissat_plain": "#ff7f0e",
    "kissat_default": "#2ca02c",
    "cryptominisat": "#d62728",
}


@dataclass(frozen=True)
class FamilySpec:
    name: str
    input_format: str
    kind: str  # random or deterministic
    x_values: Sequence[int]
    x_label: str
    filename_regex: Pattern[str]
    fit_mode: str = "power_on_x"


FAMILY_SPECS: Dict[str, FamilySpec] = {
    "random_2xnf_n": FamilySpec(
        name="random_2xnf_n",
        input_format="xnf",
        kind="random",
        x_values=[17, 18, 19, 20, 21, 22, 23, 24, 25],
        x_label="n",
        filename_regex=re.compile(r"^random_2xnf_(\d+)_\d+\.cnf$"),
    ),
    "random_3xnf_n": FamilySpec(
        name="random_3xnf_n",
        input_format="xnf",
        kind="random",
        x_values=[12, 13, 14, 15, 16, 17, 18, 19, 20],
        x_label="n",
        filename_regex=re.compile(r"^random_3xnf_(\d+)_\d+\.cnf$"),
    ),
    "random_kxnf_15": FamilySpec(
        name="random_kxnf_15",
        input_format="xnf",
        kind="random",
        x_values=[2, 3, 4, 5, 6, 7],
        x_label="k",
        filename_regex=re.compile(r"^random_kxnf_15_(\d+)_\d+\.cnf$"),
    ),
    "restricted_2xnf_n": FamilySpec(
        name="restricted_2xnf_n",
        input_format="xnf",
        kind="random",
        x_values=[30, 32, 34, 36, 38, 40, 42, 44, 46, 48, 50],
        x_label="n",
        filename_regex=re.compile(r"^restricted_2xnf_(\d+)_\d+\.cnf$"),
    ),
    "restricted_3xnf_n": FamilySpec(
        name="restricted_3xnf_n",
        input_format="xnf",
        kind="random",
        x_values=[20, 22, 24, 26, 28, 30, 32, 34, 36],
        x_label="n",
        filename_regex=re.compile(r"^restricted_3xnf_(\d+)_\d+\.cnf$"),
    ),
    "restricted_kxnf_25": FamilySpec(
        name="restricted_kxnf_25",
        input_format="xnf",
        kind="random",
        x_values=[2, 3, 4, 5, 6, 7],
        x_label="k",
        filename_regex=re.compile(r"^restricted_kxnf_25_(\d+)_\d+\.cnf$"),
    ),
    "pebbling_2lift_h": FamilySpec(
        name="pebbling_2lift_h",
        input_format="xnf",
        kind="deterministic",
        x_values=[100, 150, 200, 250, 300, 350, 400],
        x_label="h",
        filename_regex=re.compile(r"^pebbling_2lift_(\d+)\.cnf$"),
    ),
    "pebbling_3lift_h": FamilySpec(
        name="pebbling_3lift_h",
        input_format="xnf",
        kind="deterministic",
        x_values=[50, 75, 100, 125, 150, 175, 200, 225, 250],
        x_label="h",
        filename_regex=re.compile(r"^pebbling_3lift_(\d+)\.cnf$"),
    ),
    "pebbling_klift_100": FamilySpec(
        name="pebbling_klift_100",
        input_format="xnf",
        kind="deterministic",
        x_values=[2, 3, 4, 5, 6, 7],
        x_label="k",
        filename_regex=re.compile(r"^pebbling_klift_100_(\d+)\.cnf$"),
    ),
    "bijphp_n_n+1": FamilySpec(
        name="bijphp_n_n+1",
        input_format="cnf",
        kind="deterministic",
        x_values=[4, 5, 6, 7, 8],
        x_label="n",
        filename_regex=re.compile(r"^bijphp_(\d+)\.cnf$"),
    ),
    "tseitin_k_20": FamilySpec(
        name="tseitin_k_20",
        input_format="cnf",
        kind="random",
        x_values=[3, 4, 5, 6, 7, 8, 9],
        x_label="k",
        filename_regex=re.compile(r"^tseitin_k_20_(\d+)_\d+\.cnf$"),
        fit_mode="power_on_2powx",
    ),
    "tseitin_5_n": FamilySpec(
        name="tseitin_5_n",
        input_format="cnf",
        kind="random",
        x_values=[10, 20, 40, 80, 160, 320],
        x_label="n",
        filename_regex=re.compile(r"^tseitin_5_n_(\d+)_\d+\.cnf$"),
        fit_mode="power_on_x",
    ),
}

TSEITIN_FAMILIES = ("tseitin_k_20", "tseitin_5_n")


def extract_x_from_name(spec: FamilySpec, filename: str) -> Optional[int]:
    m = spec.filename_regex.match(filename)
    if not m:
        return None
    return int(m.group(1))


def collect_files_by_x(tests_root: Path, spec: FamilySpec) -> Dict[int, List[Path]]:
    family_dir = tests_root / spec.name
    if not family_dir.is_dir():
        raise FileNotFoundError(f"missing test family directory: {family_dir}")

    by_x: Dict[int, List[Path]] = {x: [] for x in spec.x_values}
    for p in sorted(family_dir.glob("*.cnf")):
        x = extract_x_from_name(spec, p.name)
        if x is None or x not in by_x:
            continue
        by_x[x].append(p)
    return by_x


def solver_line_key(row: Dict[str, str]) -> Optional[str]:
    solver = row.get("solver", "")
    config = row.get("solver_config", "")
    if solver == "xorcle_0_1":
        return "xorcle_0_1"
    if solver == "2xornado":
        return "2xornado"
    if solver == "xorricane":
        return "xorricane"
    if solver == "kissat":
        if config.startswith("kissat_plain_") or config == "kissat_cnf":
            return "kissat_plain"
        if config.startswith("kissat_default_"):
            return "kissat_default"
        return None
    if solver == "cryptominisat":
        return "cryptominisat"
    return None


def runtime_for_row(row: Dict[str, str]) -> Optional[float]:
    status = row.get("status", "")
    if status not in {"SAT", "UNSAT"}:
        return None
    if row.get("solver") == "xorcle_0_1":
        raw = row.get("parsed_solver_time_s", "") or row.get("elapsed_wall_s", "")
    else:
        raw = row.get("elapsed_wall_s", "")
    try:
        return float(raw)
    except (TypeError, ValueError):
        return None


def timeout_aware_median_runtime(rows: Sequence[Dict[str, str]]) -> Optional[float]:
    ranked: List[float] = []
    for row in rows:
        t = runtime_for_row(row)
        if t is None:
            ranked.append(math.inf)
        else:
            ranked.append(t)
    if not ranked:
        return None
    ranked.sort()
    median = ranked[len(ranked) // 2]
    if math.isinf(median):
        return None
    return median


def median_is_timeout(rows: Sequence[Dict[str, str]]) -> bool:
    ranked: List[float] = []
    for row in rows:
        t = runtime_for_row(row)
        ranked.append(math.inf if t is None else t)
    if not ranked:
        return False
    ranked.sort()
    return math.isinf(ranked[len(ranked) // 2])


def format_duration(seconds: float) -> str:
    total = max(0, int(seconds))
    h = total // 3600
    m = (total % 3600) // 60
    s = total % 60
    return f"{h:02d}:{m:02d}:{s:02d}"


def power_fit(points: Sequence[tuple[int, float]], mode: str) -> Optional[tuple[float, float, float]]:
    if len(points) < 2:
        return None
    xs = []
    ys = []
    for x, y in points:
        if x <= 0 or y <= 0:
            continue
        fit_x = (2.0 ** float(x)) if mode == "power_on_2powx" else float(x)
        xs.append(math.log(fit_x))
        ys.append(math.log(float(y)))
    if len(xs) < 2:
        return None

    mx = sum(xs) / len(xs)
    my = sum(ys) / len(ys)
    varx = sum((x - mx) ** 2 for x in xs)
    if varx <= 0:
        return None
    cov = sum((x - mx) * (y - my) for x, y in zip(xs, ys))
    p = cov / varx
    a = my - p * mx
    sst = sum((y - my) ** 2 for y in ys)
    if sst <= 0:
        r2 = 1.0
    else:
        sse = sum((y - (a + p * x)) ** 2 for x, y in zip(xs, ys))
        r2 = max(0.0, 1.0 - (sse / sst))
    return a, p, r2


def eval_power_fit(x: float, a: float, p: float, mode: str) -> float:
    fit_x = (2.0 ** x) if mode == "power_on_2powx" else x
    return math.exp(a) * (fit_x ** p)


def median(values: Sequence[float]) -> float:
    return float(statistics.median(values))
