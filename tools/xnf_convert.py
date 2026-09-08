#!/usr/bin/env python3
"""
Command-line wrapper for converting XNF files into solver-facing formats.
"""

from __future__ import annotations

import argparse
from typing import List

from xnf_lib import (
    convert_xnf_to_cnf,
    convert_xnf_to_cnf_naive,
    convert_xnf_to_cnf_xor,
    convert_xnf_to_2xnf,
    parse_xnf_file,
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
    parser = argparse.ArgumentParser(description="Convert extended DIMACS/XNF to CNF, CNF-XOR, 2-XNF, or exponential CNF.")
    parser.add_argument("format", choices=["cnf", "cnf-exp", "cnf-xor", "2-xnf"], help="Output format")
    parser.add_argument("input", help="Input XNF file")
    parser.add_argument("output", help="Output file")
    args = parser.parse_args()

    nvars, clauses = parse_xnf_file(args.input)
    if args.format == "cnf":
        cnf, _, total_vars = convert_xnf_to_cnf(nvars, clauses)
        write_cnf(args.output, total_vars, cnf)
    elif args.format == "cnf-exp":
        cnf = convert_xnf_to_cnf_naive(nvars, clauses)
        write_cnf(args.output, nvars, cnf)
    elif args.format == "cnf-xor":
        cnf, xor, _, total_vars = convert_xnf_to_cnf_xor(nvars, clauses)
        write_cnf_xor(args.output, total_vars, cnf, xor)
    else:
        total_vars, clauses_2xnf = convert_xnf_to_2xnf(nvars, clauses)
        write_xnf(args.output, total_vars, clauses_2xnf)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
