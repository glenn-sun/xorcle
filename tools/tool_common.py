#!/usr/bin/env python3
from __future__ import annotations

from pathlib import Path
from typing import List, Sequence


def repo_root() -> Path:
    return Path(__file__).resolve().parents[1]


def default_solver_bin(root: Path) -> Path:
    release = root / "bin" / "xorcle"
    if release.exists():
        return release
    debug = root / "bin" / "xorcle-debug"
    if debug.exists():
        return debug
    raise FileNotFoundError("No default solver binary found at bin/xorcle or bin/xorcle-debug")


def parse_csv_choices(raw: str, valid: Sequence[str], label: str) -> List[str]:
    selected = [item.strip() for item in raw.split(",") if item.strip()]
    if not selected:
        raise ValueError(f"{label} must not be empty")
    unknown = [item for item in selected if item not in valid]
    if unknown:
        valid_str = ", ".join(valid)
        unknown_str = ", ".join(unknown)
        raise ValueError(f"unknown {label}: {unknown_str}; valid values: {valid_str}")
    return selected
