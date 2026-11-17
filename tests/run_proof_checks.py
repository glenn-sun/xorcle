#!/usr/bin/env python3
"""Run every row of tests/proof_checks.csv through tools/run_with_proof_check.sh.

Each row names an UNSAT instance under tests/instances/, the script's options, the
solver's options, the statistics that must end above zero, and what the row is for.
Every failing row is reported by name, and the exit code is 1 if any failed.
"""

from __future__ import annotations

import argparse
import csv
import shlex
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

TESTS_DIR = Path(__file__).resolve().parent
ROW_TIMEOUT_S = 300


def run_row(row: dict[str, str], args: argparse.Namespace) -> tuple[str, bool, float, str]:
    command = ["sh", str(args.root / "tools" / "run_with_proof_check.sh")]
    command += shlex.split(row["script_options"])
    for statistic in row["statistics"].split():
        command += ["--expect-stat", statistic]
    command += [sys.executable, args.xorcle, args.checker,
                str(TESTS_DIR / "instances" / row["instance"])]
    command += shlex.split(row["solver_options"]) + args.solver_options
    start = time.monotonic()
    try:
        result = subprocess.run(command, capture_output=True, text=True, timeout=ROW_TIMEOUT_S)
        passed = result.returncode == 0
        output = result.stdout + result.stderr
    except subprocess.TimeoutExpired:
        passed = False
        output = f"timed out after {ROW_TIMEOUT_S} s\n"
    return row["name"], passed, time.monotonic() - start, output


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("root", type=Path, help="the repository root")
    parser.add_argument("xorcle", help="the solver binary")
    parser.add_argument("checker", help="the check_lrup_parity binary")
    parser.add_argument("--jobs", type=int, default=4, help="rows run at once")
    parser.add_argument("--rows", help="comma-separated names of the rows to run (default: all)")
    parser.add_argument("solver_options", nargs="*",
                        help="after --, options added to every row's solver options")
    args = parser.parse_args()

    with open(TESTS_DIR / "proof_checks.csv", newline="", encoding="utf-8") as f:
        rows = list(csv.DictReader(f))
    if args.rows is not None:
        wanted = args.rows.split(",")
        unknown = set(wanted) - {row["name"] for row in rows}
        if unknown:
            parser.error(f"no rows named {', '.join(sorted(unknown))}")
        rows = [row for row in rows if row["name"] in wanted]

    failed = []
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        for name, passed, seconds, output in pool.map(lambda row: run_row(row, args), rows):
            print(f"{'ok  ' if passed else 'FAIL'} {name} ({seconds:.1f} s)", flush=True)
            if not passed:
                failed.append(name)
                print("".join(f"    {line}\n" for line in output.splitlines()[-20:]), end="")
    if failed:
        print(f"{len(failed)} of {len(rows)} rows failed: {', '.join(failed)}")
        return 1
    print(f"all {len(rows)} rows passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
