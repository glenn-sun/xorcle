#!/usr/bin/env python3
from __future__ import annotations

import csv
import hashlib
import math
import statistics
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional, Sequence

ALL_SOLVER_LINES = (
    "xorcle",
    "kissat_default",
    "kissat_plain",
    "cryptominisat",
    "2xornado",
)

DEFAULT_SOLVER_LINES = ALL_SOLVER_LINES

SOLVER_TO_BENCH_ARG = {
    "xorcle": "xorcle",
    "2xornado": "2xornado",
    "kissat_plain": "kissat_plain",
    "kissat_default": "kissat_default",
    "cryptominisat": "cryptominisat",
}

SOLVER_DISPLAY = {
    "xorcle": "Xorcle 0.1",
    "2xornado": "2-Xornado 1.0.2",
    "kissat_plain": "Kissat --plain 4.0.4",
    "kissat_default": "Kissat 4.0.4",
    "cryptominisat": "CryptoMiniSAT 5.13.0",
}

SOLVER_COLORS = {
    "xorcle": "#1f77b4",
    "2xornado": "#9467bd",
    "kissat_plain": "#ff7f0e",
    "kissat_default": "#2ca02c",
    "cryptominisat": "#d62728",
}

GENERATED_FAMILIES = (
    "random_kxnf_n",
    "restricted_kxnf_n",
    "pebbling_lifted_h_k",
    "tseitin_n_k",
)

PHYSICAL_FAMILIES = (
    "generated/random_kxnf_n",
    "generated/restricted_kxnf_n",
    "generated/pebbling_lifted_h_k",
    "generated/tseitin_n_k",
    "ascon",
    "sat-comp",
)

REPRESENTATIVE_FAMILIES = (
    "generated/random_kxnf_n",
    "generated/restricted_kxnf_n",
    "generated/pebbling_lifted_h_k",
    "generated/tseitin_n_k",
    "ascon",
)

TYPE2_VIEWS = ("tseitin_k_20", "tseitin_4_n")

MANIFEST_FIELDS = ("file", "family", "input_format", "attempt", "n", "k", "h", "lift_k")


@dataclass(frozen=True)
class InstanceRecord:
    family: str
    file: Path
    input_format: str
    attempt: int
    n: Optional[int] = None
    k: Optional[int] = None
    h: Optional[int] = None
    lift_k: Optional[int] = None


@dataclass(frozen=True)
class PhysicalFamilySpec:
    name: str
    manifest_family: Optional[str] = None
    recursive_glob: Optional[str] = None
    input_format: Optional[str] = None


@dataclass(frozen=True)
class Type2ViewSpec:
    name: str
    x_label: str
    x_field: str
    x_values: Sequence[int]
    fixed_filters: Dict[str, int]
    fit_mode: str


PHYSICAL_FAMILY_SPECS: Dict[str, PhysicalFamilySpec] = {
    "generated/random_kxnf_n": PhysicalFamilySpec(
        name="generated/random_kxnf_n",
        manifest_family="random_kxnf_n",
    ),
    "generated/restricted_kxnf_n": PhysicalFamilySpec(
        name="generated/restricted_kxnf_n",
        manifest_family="restricted_kxnf_n",
    ),
    "generated/pebbling_lifted_h_k": PhysicalFamilySpec(
        name="generated/pebbling_lifted_h_k",
        manifest_family="pebbling_lifted_h_k",
    ),
    "generated/tseitin_n_k": PhysicalFamilySpec(
        name="generated/tseitin_n_k",
        manifest_family="tseitin_n_k",
    ),
    "ascon": PhysicalFamilySpec(
        name="ascon",
        recursive_glob="*.xnf",
        input_format="xnf",
    ),
    "sat-comp": PhysicalFamilySpec(
        name="sat-comp",
        recursive_glob="*.cnf",
        input_format="cnf",
    ),
}

TYPE2_VIEW_SPECS: Dict[str, Type2ViewSpec] = {
    "tseitin_k_20": Type2ViewSpec(
        name="tseitin_k_20",
        x_label="k",
        x_field="k",
        x_values=[3, 4, 5, 6, 7, 8, 9, 10],
        fixed_filters={"n": 20},
        fit_mode="power_on_2powx",
    ),
    "tseitin_4_n": Type2ViewSpec(
        name="tseitin_4_n",
        x_label="n",
        x_field="n",
        x_values=[10, 20, 40, 80, 160, 320, 640, 1280],
        fixed_filters={"k": 4},
        fit_mode="power_on_x",
    ),
}


def family_slug(name: str) -> str:
    return name.replace("/", "__")


def parse_optional_int(raw: str | None) -> Optional[int]:
    text = (raw or "").strip()
    if not text:
        return None
    return int(text)


def read_csv_rows(path: Path) -> List[Dict[str, str]]:
    with path.open("r", encoding="utf-8", newline="") as f:
        return list(csv.DictReader(f))


def generated_manifest_path(tests_root: Path, generated_family: str) -> Path:
    return tests_root / "generated" / generated_family / "manifest.csv"


def load_generated_family_instances(tests_root: Path, generated_family: str, physical_name: str) -> List[InstanceRecord]:
    manifest_path = generated_manifest_path(tests_root, generated_family)
    if not manifest_path.is_file():
        raise FileNotFoundError(f"missing generated family manifest: {manifest_path}")

    rows = read_csv_rows(manifest_path)
    out: List[InstanceRecord] = []
    for row in rows:
        file_rel = (row.get("file") or "").strip()
        if not file_rel:
            raise RuntimeError(f"{manifest_path}: manifest row missing file")
        file_path = (tests_root / file_rel).resolve()
        if not file_path.is_file():
            raise FileNotFoundError(f"missing manifest file entry: {file_path}")
        input_format = (row.get("input_format") or "").strip().lower()
        if input_format not in {"cnf", "xnf"}:
            raise RuntimeError(f"{manifest_path}: invalid input_format '{input_format}' for {file_rel}")
        attempt = parse_optional_int(row.get("attempt")) or 1
        out.append(
            InstanceRecord(
                family=physical_name,
                file=file_path,
                input_format=input_format,
                attempt=attempt,
                n=parse_optional_int(row.get("n")),
                k=parse_optional_int(row.get("k")),
                h=parse_optional_int(row.get("h")),
                lift_k=parse_optional_int(row.get("lift_k")),
            )
        )
    out.sort(key=lambda r: (str(r.file), r.attempt))
    return out


def load_physical_family_instances(tests_root: Path, family_name: str) -> List[InstanceRecord]:
    if family_name not in PHYSICAL_FAMILY_SPECS:
        raise KeyError(f"unknown physical family: {family_name}")
    spec = PHYSICAL_FAMILY_SPECS[family_name]
    if spec.manifest_family is not None:
        return load_generated_family_instances(tests_root, spec.manifest_family, spec.name)

    root_dir = tests_root / family_name
    if not root_dir.is_dir():
        raise FileNotFoundError(f"missing family directory: {root_dir}")
    pattern = spec.recursive_glob or "*.cnf"
    input_format = spec.input_format or "cnf"
    files = sorted(p.resolve() for p in root_dir.rglob(pattern) if p.is_file())
    if not files:
        raise RuntimeError(f"no files matched {pattern} under {root_dir}")
    return [
        InstanceRecord(
            family=family_name,
            file=file_path,
            input_format=input_format,
            attempt=1,
        )
        for file_path in files
    ]


def load_physical_families(
    tests_root: Path,
    family_names: Sequence[str] | None = None,
) -> Dict[str, List[InstanceRecord]]:
    names = list(family_names) if family_names else list(PHYSICAL_FAMILIES)
    return {name: load_physical_family_instances(tests_root, name) for name in names}


def load_representative_sample(
    tests_root: Path,
    *,
    count_per_family: int,
    seed: int,
) -> Dict[str, List[InstanceRecord]]:
    return sample_grouped_instances(
        load_physical_families(tests_root, REPRESENTATIVE_FAMILIES),
        count_per_family=count_per_family,
        seed=seed,
        cohort_prefix="family",
    )


def load_type2_view_instances(generated_root: Path, view_name: str) -> tuple[Type2ViewSpec, Dict[int, List[InstanceRecord]]]:
    if view_name not in TYPE2_VIEW_SPECS:
        raise KeyError(f"unknown type2 view: {view_name}")
    view = TYPE2_VIEW_SPECS[view_name]
    tests_root = generated_root.parent.resolve()
    records = load_generated_family_instances(tests_root, "tseitin_n_k", "generated/tseitin_n_k")
    grouped: Dict[int, List[InstanceRecord]] = {x: [] for x in view.x_values}

    for record in records:
        keep = True
        for field, expected in view.fixed_filters.items():
            value = getattr(record, field)
            if value != expected:
                keep = False
                break
        if not keep:
            continue
        x_value = getattr(record, view.x_field)
        if x_value in grouped:
            grouped[int(x_value)].append(record)

    for x in view.x_values:
        grouped[x].sort(key=lambda r: (str(r.file), r.attempt))
    return view, grouped


def sample_instances(
    records: Sequence[InstanceRecord],
    *,
    count: int,
    seed: int,
    cohort_id: str,
) -> List[InstanceRecord]:
    items = ordered_instances(records, seed=seed, cohort_id=cohort_id)
    if count < 1:
        raise ValueError("count must be >= 1")
    if not items:
        raise RuntimeError("cannot sample from an empty instance list")
    if count >= len(items):
        return items
    return items[:count]


def sample_grouped_instances(
    families: Dict[str, List[InstanceRecord]],
    *,
    count_per_family: int,
    seed: int,
    cohort_prefix: str,
) -> Dict[str, List[InstanceRecord]]:
    return {
        family: sample_instances(
            records,
            count=count_per_family,
            seed=seed,
            cohort_id=f"{cohort_prefix}:{family}",
        )
        for family, records in families.items()
    }


def instance_sort_key(record: InstanceRecord) -> tuple[str, int, str]:
    return (str(record.file), record.attempt, record.input_format)


def ordered_instances(
    records: Sequence[InstanceRecord],
    *,
    seed: int,
    cohort_id: str,
) -> List[InstanceRecord]:
    ordered_records = sorted(records, key=instance_sort_key)

    def stable_rank(record: InstanceRecord) -> str:
        token = f"{seed}:{cohort_id}:{record.file}:{record.attempt}:{record.input_format}"
        return hashlib.sha256(token.encode("utf-8")).hexdigest()

    return sorted(ordered_records, key=lambda record: (stable_rank(record), instance_sort_key(record)))


def sample_key_for(*, family: str, index: int, file_path: Path) -> str:
    return f"{family_slug(family)}:{index:04d}:{file_path.name}"


def manifest_rows_for_grouped_instances(grouped_records: Dict[str, List[InstanceRecord]]) -> List[Dict[str, str]]:
    rows: List[Dict[str, str]] = []
    for family, records in grouped_records.items():
        for index, record in enumerate(records, start=1):
            rows.append(
                {
                    "family": family,
                    "sample_index": str(index),
                    "sample_key": sample_key_for(family=family, index=index, file_path=record.file),
                    "file": str(record.file),
                    "input_format": record.input_format,
                    "attempt": str(record.attempt),
                    "n": "" if record.n is None else str(record.n),
                    "k": "" if record.k is None else str(record.k),
                    "h": "" if record.h is None else str(record.h),
                    "lift_k": "" if record.lift_k is None else str(record.lift_k),
                }
            )
    return rows


def aggregation_key(row: Dict[str, str]) -> str:
    sample_key = (row.get("sample_key") or "").strip()
    if sample_key:
        return sample_key
    file_key = (row.get("file") or "").strip()
    if file_key:
        return file_key
    return "<missing-file>"


def solver_line_key(row: Dict[str, str]) -> Optional[str]:
    solver = row.get("solver", "")
    config = row.get("solver_config", "")
    if solver == "xorcle":
        return "xorcle"
    if solver == "2xornado":
        return "2xornado"
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
    raw = row.get("elapsed_wall_s", "")
    try:
        return float(raw)
    except (TypeError, ValueError):
        return None


def decisions_for_row(row: Dict[str, str]) -> Optional[float]:
    status = row.get("status", "")
    if status not in {"SAT", "UNSAT"}:
        return None
    raw = row.get("parsed_solver_decisions", "")
    try:
        value = float(raw)
    except (TypeError, ValueError):
        return None
    return value if value > 0.0 else None


def timeout_aware_median_runtime(rows: Sequence[Dict[str, str]]) -> Optional[float]:
    ranked: List[float] = []
    for row in rows:
        t = runtime_for_row(row)
        ranked.append(math.inf if t is None else t)
    if not ranked:
        return None
    ranked.sort()
    median = ranked[len(ranked) // 2]
    return None if math.isinf(median) else median


def timeout_aware_median_decisions(rows: Sequence[Dict[str, str]]) -> Optional[float]:
    ranked: List[float] = []
    for row in rows:
        d = decisions_for_row(row)
        ranked.append(math.inf if d is None else d)
    if not ranked:
        return None
    ranked.sort()
    median = ranked[len(ranked) // 2]
    return None if math.isinf(median) else median


def median(values: Sequence[float]) -> float:
    return float(statistics.median(values))


def format_duration(seconds: float) -> str:
    total = max(0, int(seconds))
    h = total // 3600
    m = (total % 3600) // 60
    s = total % 60
    return f"{h:02d}:{m:02d}:{s:02d}"


def should_report_progress(done: int, total: int, every: int) -> bool:
    if done <= 0:
        return False
    if total > 0 and done >= total:
        return True
    step = max(1, int(every))
    return done % step == 0


def format_progress_line(*, done: int, total: int, start_s: float, **context: object) -> str:
    elapsed = time.perf_counter() - start_s
    pct = 100.0 * done / total if total else 100.0
    eta = 0.0
    if done > 0 and total > done:
        eta = (elapsed / done) * (total - done)

    parts = [
        f"[progress] {pct:5.1f}% ({done}/{total})",
        f"elapsed={format_duration(elapsed)}",
        f"eta={format_duration(eta)}",
    ]
    for key, value in context.items():
        if value is None:
            continue
        text = str(value).strip()
        if text:
            parts.append(f"{key}={text}")
    return " ".join(parts)


def power_fit(points: Sequence[tuple[int, float]], mode: str) -> Optional[tuple[float, float, float]]:
    if len(points) < 2:
        return None
    xs: List[float] = []
    ys: List[float] = []
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
