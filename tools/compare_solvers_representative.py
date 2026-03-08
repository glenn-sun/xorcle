#!/usr/bin/env python3
from __future__ import annotations

import sys


def main() -> int:
    print("This script is deprecated.")
    print("Use one of:")
    print("  python3 tools/benchmark_type1.py")
    print("  python3 tools/benchmark_type2_tseitin_powerlaw.py")
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
