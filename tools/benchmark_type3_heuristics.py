#!/usr/bin/env python3
from __future__ import annotations

import argparse
import csv
import os
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass
from pathlib import Path
from types import SimpleNamespace
from typing import Dict, List, Sequence, Tuple

os.environ.setdefault("MPLCONFIGDIR", str(Path("/tmp") / "parity-sat-cpp-mplconfig"))

sys.path.append(str(Path(__file__).resolve().parent))
import run_benchmark as rb


def repo_root() -> Path:
    return Path(__file__).resolve().parents[1]


DECISION_PROCS = ("vsids_unit", "cvsids")
CLAUSE_SELECTORS = ("cyclic", "lt_watchlist", "exact_watchlist")
DECISION_PHASES = ("sat", "unsat", "random")
ALL_TEST_IDS = ("1a", "1b", "2a", "2b", "3a", "3b")


@dataclass(frozen=True)
class VariantSpec:
    label: str
    solver_passthrough: Tuple[str, ...]


@dataclass(frozen=True)
class TestSpec:
    test_id: str
    title: str
    dataset: str  # satcomp or tests
    variants: Tuple[VariantSpec, ...]


TEST_SPECS: Dict[str, TestSpec] = {
    "1a": TestSpec(
        test_id="1a",
        title="Type 3 Test 1a: Decision Procedures on sat-comp-tests",
        dataset="satcomp",
        variants=tuple(VariantSpec(label=v, solver_passthrough=("-d", v)) for v in DECISION_PROCS),
    ),
    "1b": TestSpec(
        test_id="1b",
        title="Type 3 Test 1b: Decision Procedures on tests samples",
        dataset="tests",
        variants=tuple(VariantSpec(label=v, solver_passthrough=("-d", v)) for v in DECISION_PROCS),
    ),
    "2a": TestSpec(
        test_id="2a",
        title="Type 3 Test 2a: Clause Selectors on sat-comp-tests",
        dataset="satcomp",
        variants=tuple(VariantSpec(label=v, solver_passthrough=("-c", v)) for v in CLAUSE_SELECTORS),
    ),
    "2b": TestSpec(
        test_id="2b",
        title="Type 3 Test 2b: Clause Selectors on tests samples",
        dataset="tests",
        variants=tuple(VariantSpec(label=v, solver_passthrough=("-c", v)) for v in CLAUSE_SELECTORS),
    ),
    "3a": TestSpec(
        test_id="3a",
        title="Type 3 Test 3a: Decision Phases on sat-comp-tests",
        dataset="satcomp",
        variants=tuple(VariantSpec(label=v, solver_passthrough=("-p", v)) for v in DECISION_PHASES),
    ),
    "3b": TestSpec(
        test_id="3b",
        title="Type 3 Test 3b: Decision Phases on tests samples",
        dataset="tests",
        variants=tuple(VariantSpec(label=v, solver_passthrough=("-p", v)) for v in DECISION_PHASES),
    ),
}


def parse_args() -> argparse.Namespace:
    root = repo_root()
    parser = argparse.ArgumentParser(description="Type 3 benchmark: heuristic sweeps for xorcle + cactus plots.")
    parser.add_argument("--tests-root", default=str(root / "tests"))
    parser.add_argument("--sat-comp-root", default=str(root / "sat-comp-tests"))
    parser.add_argument("--output-root", default=str(root / "benchmarks" / "type3"))
    parser.add_argument("--solver-bin", default=str(root / "bin" / "main-release"))
    parser.add_argument("--2xornado-bin", dest="xornado_bin", default=str(root / "external" / "2xornado"))
    parser.add_argument("--xorricane-bin", default=str(root / "external" / "xorricane"))
    parser.add_argument("--kissat-bin", default=str(root / "external" / "kissat"))
    parser.add_argument("--cms-bin", default=str(root / "external" / "cryptominisat5"))
    parser.add_argument("--timeout", type=float, default=5.0)
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--samples-per-family", type=int, default=3)
    parser.add_argument("--saveconverted", action="store_true")
    parser.add_argument("--only-tests", nargs="*", choices=ALL_TEST_IDS, default=list(ALL_TEST_IDS))
    args = parser.parse_args()

    if args.timeout <= 0:
        parser.error("--timeout must be > 0")
    if args.jobs < 1:
        parser.error("--jobs must be >= 1")
    if args.samples_per_family < 1:
        parser.error("--samples-per-family must be >= 1")
    return args


def write_csv(path: Path, rows: Sequence[Dict[str, str]], fieldnames: Sequence[str]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fieldnames)
        w.writeheader()
        w.writerows(rows)


def sniff_input_format(path: Path) -> str:
    try:
        with path.open("r", encoding="utf-8", errors="replace") as f:
            for raw in f:
                line = raw.strip()
                if not line or line.startswith("c") or line.startswith("p"):
                    continue
                if "+" in line or line.startswith("x"):
                    return "xnf"
                break
    except OSError:
        pass
    return "cnf"


def make_runner_args(args: argparse.Namespace, input_format: str) -> SimpleNamespace:
    return SimpleNamespace(
        input_format=input_format,
        repeat=1,
        timeout=args.timeout,
        solver_bin=str(Path(args.solver_bin).resolve()),
        kissat_bin=str(Path(args.kissat_bin).resolve()),
        cms_bin=str(Path(args.cms_bin).resolve()),
        xornado_bin=str(Path(args.xornado_bin).resolve()),
        xorricane_bin=str(Path(args.xorricane_bin).resolve()),
        csv_path="",
        kissat_mode="both",
        exp_cnf=False,
        keep_going=True,
        jobs=1,
        solvers=["xorcle_0_1"],
        saveconverted=bool(args.saveconverted),
    )


def collect_satcomp_files(root: Path) -> List[Path]:
    if not root.is_dir():
        raise FileNotFoundError(f"sat-comp root not found: {root}")
    files = sorted(p.resolve() for p in root.rglob("*.cnf") if p.is_file())
    if not files:
        raise RuntimeError(f"No .cnf files found under sat-comp root: {root}")
    return files


def collect_test_family_dirs(tests_root: Path) -> List[Path]:
    if not tests_root.is_dir():
        raise FileNotFoundError(f"tests root not found: {tests_root}")
    dirs: List[Path] = []
    for child in sorted(tests_root.iterdir()):
        if not child.is_dir():
            continue
        if any(p.is_file() for p in child.glob("*.cnf")):
            dirs.append(child.resolve())
    if not dirs:
        raise RuntimeError(f"No family directories with direct .cnf files found under: {tests_root}")
    return dirs


def choose_quantile_indices(total_files: int, k: int) -> List[Tuple[int, float]]:
    if total_files <= 0:
        return []
    target_count = min(k, total_files)
    ps = [i / float(k + 1) for i in range(1, k + 1)]
    base = [int(round(p * (total_files - 1))) for p in ps]

    chosen: List[Tuple[int, float]] = []
    used: set[int] = set()

    for idx, p in zip(base, ps):
        if len(chosen) >= target_count:
            break
        if idx not in used:
            used.add(idx)
            chosen.append((idx, p))

    cursor = 0
    while len(chosen) < target_count and ps:
        target_idx = base[cursor % len(base)]
        p = ps[cursor % len(ps)]
        unselected = [j for j in range(total_files) if j not in used]
        if not unselected:
            break
        nearest = min(unselected, key=lambda j: (abs(j - target_idx), j))
        used.add(nearest)
        chosen.append((nearest, p))
        cursor += 1

    return chosen


def collect_test_sample_files(tests_root: Path, k: int) -> tuple[List[Path], List[Dict[str, str]]]:
    files: List[Path] = []
    manifest: List[Dict[str, str]] = []

    for family_dir in collect_test_family_dirs(tests_root):
        family_files = sorted(p.resolve() for p in family_dir.glob("*.cnf") if p.is_file())
        picks = choose_quantile_indices(len(family_files), k)
        for rank, (idx, p) in enumerate(picks, start=1):
            file_path = family_files[idx]
            files.append(file_path)
            manifest.append(
                {
                    "family": family_dir.name,
                    "file": str(file_path),
                    "rank_index": str(rank),
                    "total_files": str(len(family_files)),
                    "quantile_target": f"{p:.6f}",
                }
            )
    if not files:
        raise RuntimeError("No files selected from tests families")
    return files, manifest


def run_variant(
    *,
    args: argparse.Namespace,
    files: Sequence[Path],
    solver_passthrough: Sequence[str],
    converter_script: Path,
) -> List[Dict[str, str]]:
    def run_file(file_path: Path) -> List[Dict[str, str]]:
        input_format = sniff_input_format(file_path)
        runner_args = make_runner_args(args, input_format)
        return rb.benchmark_file(file_path, runner_args, solver_passthrough, converter_script)

    rows: List[Dict[str, str]] = []
    with ThreadPoolExecutor(max_workers=args.jobs) as executor:
        futures = [executor.submit(run_file, p) for p in files]
        for future in as_completed(futures):
            rows.extend(future.result())

    rows.sort(
        key=lambda row: (
            row.get("file", ""),
            int(row.get("run_index", "0") or 0),
            row.get("solver_config", ""),
            row.get("backend_input_kind", ""),
            row.get("solver", ""),
        )
    )
    return rows


def generate_cactus_plot(
    *,
    plot_script: Path,
    output_path: Path,
    title: str,
    inputs: Sequence[tuple[str, Path]],
) -> None:
    output_path.parent.mkdir(parents=True, exist_ok=True)
    cmd = [sys.executable, str(plot_script)]
    for label, csv_path in inputs:
        cmd.extend(["--input", f"{csv_path}::{label}"])
    cmd.extend(
        [
            "--where",
            "solver=xorcle_0_1",
            "--aggregate-per-file",
            "median",
            "--title",
            title,
            "--output",
            str(output_path),
        ]
    )
    cp = subprocess.run(cmd, capture_output=True, text=True)
    if cp.returncode != 0:
        tail = (cp.stderr or cp.stdout or "")[-800:]
        raise RuntimeError(f"plot generation failed for {output_path.name}: {tail}")


def main() -> int:
    args = parse_args()
    root = repo_root()
    tests_root = Path(args.tests_root).resolve()
    sat_comp_root = Path(args.sat_comp_root).resolve()
    output_root = Path(args.output_root).resolve()
    csv_root = output_root / "csv"
    plots_root = output_root / "plots"
    converter_script = root / "tools" / "xnf_convert.py"
    plot_script = root / "tools" / "plot_cactus.py"

    csv_root.mkdir(parents=True, exist_ok=True)
    plots_root.mkdir(parents=True, exist_ok=True)

    satcomp_files = collect_satcomp_files(sat_comp_root)
    test_sample_files, sample_manifest = collect_test_sample_files(tests_root, args.samples_per_family)
    manifest_path = csv_root / "selected_instances.csv"
    write_csv(
        manifest_path,
        sample_manifest,
        ["family", "file", "rank_index", "total_files", "quantile_target"],
    )
    print(f"[type3] wrote sample manifest: {manifest_path}")

    for test_id in args.only_tests:
        test_spec = TEST_SPECS[test_id]
        files = satcomp_files if test_spec.dataset == "satcomp" else test_sample_files
        test_csv_dir = csv_root / test_id
        test_csv_dir.mkdir(parents=True, exist_ok=True)

        plot_inputs: List[tuple[str, Path]] = []
        print(f"[type3] running {test_id}: {test_spec.title} ({len(files)} files)")
        for variant in test_spec.variants:
            rows = run_variant(
                args=args,
                files=files,
                solver_passthrough=list(variant.solver_passthrough),
                converter_script=converter_script,
            )
            csv_path = test_csv_dir / f"{variant.label}.raw.csv"
            write_csv(csv_path, rows, rb.RAW_CSV_FIELDS)
            plot_inputs.append((variant.label, csv_path))
            print(f"[type3] {test_id}/{variant.label}: wrote {csv_path} ({len(rows)} rows)")

        plot_path = plots_root / f"{test_id}.png"
        generate_cactus_plot(
            plot_script=plot_script,
            output_path=plot_path,
            title=test_spec.title,
            inputs=plot_inputs,
        )
        print(f"[type3] {test_id}: wrote plot {plot_path}")

    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (FileNotFoundError, RuntimeError, ValueError) as exc:
        print(f"Error: {exc}", file=sys.stderr)
        raise SystemExit(2)
