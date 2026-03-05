#!/usr/bin/env python3
"""
Convert DIMACS-plus (XNF) files to CNF, CNF-XOR, or 2-XNF.

CLI usage:
  python3 tools/xnf_convert.py <format> <input.xnf> <output>
Where <format> is one of: cnf, cnf-xor, 2-xnf

Library usage:
  from tools.xnf_convert import parse_xnf_file, convert_xnf, write_cnf, write_cnf_xor
"""

from __future__ import annotations

import argparse
from typing import List

import os
import sys

sys.path.append(os.path.dirname(__file__))

from xnf_lib import (
    parse_xnf_file,
    convert_xnf_to_cnf,
    convert_xnf_to_cnf_xor,
    convert_xnf_to_2xnf,
    write_cnf,
    write_xnf,
)


def write_cnf_xor(path: str, nvars: int, cnf_clauses: List[List[int]], xor_clauses: List[List[int]]) -> None:
    with open(path, "w", encoding="utf-8") as f:
        f.write(f"p cnf {nvars} {len(cnf_clauses)}\n")
        for xor in xor_clauses:
            if not xor:
                continue
            line = "x" + str(xor[0])
            for lit in xor[1:]:
                line += f" {lit}"
            f.write(line + " 0\n")
        for clause in cnf_clauses:
            if clause:
                f.write(" ".join(str(l) for l in clause) + " 0\n")
            else:
                f.write("0\n")


def main() -> int:
    parser = argparse.ArgumentParser(description="Convert XNF (DIMACS-plus) to CNF, CNF-XOR, or 2-XNF.")
    parser.add_argument("format", choices=["cnf", "cnf-xor", "2-xnf"], help="Output format")
    parser.add_argument("input", help="Input XNF file")
    parser.add_argument("output", help="Output file")
    args = parser.parse_args()

    nvars, clauses = parse_xnf_file(args.input)
    if args.format == "cnf":
        cnf, _, total_vars = convert_xnf_to_cnf(nvars, clauses)
        write_cnf(args.output, total_vars, cnf)
    elif args.format == "cnf-xor":
        cnf, xor, _, total_vars = convert_xnf_to_cnf_xor(nvars, clauses)
        write_cnf_xor(args.output, total_vars, cnf, xor)
    else:
        total_vars, clauses_2xnf = convert_xnf_to_2xnf(nvars, clauses)
        write_xnf(args.output, total_vars, clauses_2xnf)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
