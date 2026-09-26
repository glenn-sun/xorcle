#!/usr/bin/env python3
"""
LRUP(⊕) proof checker for the extended DIMACS/XNF format, CNF-XOR lines included.

Usage:
  python3 tools/check_lrup_parity.py <input.xnf> <proof.lrup>
"""

import argparse
import sys
from typing import Dict, List, Tuple


class Basis:
    """Row-echelon basis over GF(2) using int bitsets."""

    def __init__(self, nvars: int) -> None:
        self.nvars = nvars
        self.rows: Dict[int, int] = {}
        self.pivots: List[int] = []

    def reduce(self, row: int) -> int:
        for pivot in self.pivots:
            if (row >> pivot) & 1:
                row ^= self.rows[pivot]
        return row

    def contains(self, row: int) -> bool:
        return self.reduce(row) == 0

    def add(self, row: int) -> bool:
        row = self.reduce(row)
        if row == 0:
            return False
        pivot = row.bit_length() - 1
        assert pivot not in self.rows, "reduce left a row on an existing pivot"
        self.rows[pivot] = row
        # Insert pivot in descending order.
        idx = 0
        while idx < len(self.pivots) and self.pivots[idx] > pivot:
            idx += 1
        self.pivots.insert(idx, pivot)
        return True


def parse_header(lines: List[str]) -> Tuple[int, int, int]:
    for idx, line in enumerate(lines):
        stripped = line.strip()
        if not stripped or stripped.startswith("c"):
            continue
        if stripped.startswith("p"):
            parts = stripped.split()
            if len(parts) < 4 or parts[1] not in {"cnf", "xnf"}:
                raise ValueError(f"Invalid header: {stripped}")
            try:
                nvars = int(parts[2])
                nclauses = int(parts[3])
            except Exception as exc:
                raise ValueError(f"Invalid header: {stripped}") from exc
            return idx, nvars, nclauses
        raise ValueError(f"Expected header, got: {stripped}")
    raise ValueError("No header found")


def parse_clause_token(token: str, nvars: int) -> int:
    rhs_zero = False
    body = token
    if token.startswith("-"):
        rhs_zero = True
        body = token[1:]
        if not body:
            raise ValueError(f"Invalid token: '{token}'")

    if "-" in body:
        raise ValueError(f"Illegal '-' inside token: '{token}'")

    if "+" in body:
        parts = body.split("+")
    else:
        parts = [body]

    row = 0
    for part in parts:
        if part == "":
            raise ValueError(f"Empty variable in token: '{token}'")
        try:
            var = int(part)
        except Exception as exc:
            raise ValueError(f"Invalid integer in token: '{token}'") from exc
        if var <= 0 or var > nvars:
            raise ValueError(f"Variable out of range in token: '{token}'")
        row ^= 1 << (var - 1)  # addition, so a repeated variable cancels

    if rhs_zero:
        row |= 1 << nvars
    return row


def parse_xor_line(body: str, nvars: int) -> int:
    """The stored row of a CNF-XOR line `x<lit> ... 0`, its leading `x` removed.

    The literals XOR to 1, so the stored linear negation has the constant bit
    set when an odd number of literals are negated.
    """
    tokens = body.split()
    if not tokens or tokens[-1] != "0" or "0" in tokens[:-1]:
        raise ValueError(f"XOR line must end with its only 0: 'x{body}'")
    row = 0
    for tok in tokens[:-1]:
        try:
            lit = int(tok)
        except Exception as exc:
            raise ValueError(f"Invalid literal in XOR line: '{tok}'") from exc
        if lit == 0 or abs(lit) > nvars:
            raise ValueError(f"Variable out of range in XOR line: '{tok}'")
        row ^= 1 << (abs(lit) - 1)
        if lit < 0:
            row ^= 1 << nvars
    return row


def parse_xnf_file(path: str) -> Tuple[int, Dict[int, List[int]]]:
    with open(path, "r", encoding="utf-8") as f:
        lines = f.readlines()

    header_idx, nvars, _ = parse_header(lines)
    clauses: Dict[int, List[int]] = {}
    current: List[str] = []
    for line in lines[header_idx + 1 :]:
        stripped = line.strip()
        if not stripped or stripped.startswith("c"):
            continue
        if stripped.startswith("x"):
            if current:
                raise ValueError("Clause missing trailing 0 before an XOR line")
            clauses[len(clauses) + 1] = [parse_xor_line(stripped[1:], nvars)]
            continue
        for tok in stripped.split():
            if tok == "0":
                clauses[len(clauses) + 1] = [parse_clause_token(t, nvars) for t in current]
                current = []
            else:
                current.append(tok)
    if current:
        raise ValueError("Last clause missing trailing 0")
    return nvars, clauses


def propagate_clause(clause_rows: List[int], basis: Basis, nvars: int):
    reduced = [basis.reduce(r) for r in clause_rows]
    nonzero = [r for r in reduced if r != 0]
    if not nonzero:
        return "conflict", None
    uniq = set(nonzero)
    if len(uniq) == 1:
        g = nonzero[0]
        f = g ^ (1 << nvars)
        if basis.contains(f):
            return "none", None
        basis.add(f)
        return "deduced", f
    return "none", None


def parse_ids(tokens: List[str]) -> List[int]:
    ids = [int(t) for t in tokens]
    for cid in ids:
        if cid <= 0:
            raise ValueError(f"Clause id must be positive: {cid}")
    return ids


def check_proof(xnf_path: str, proof_path: str, verbose: bool) -> int:
    """Check every line, and fail unless some learned line derives the empty clause.

    A learned line is `<new id> <xor expr> ... 0 <reason id> ... 0`, with exactly two
    `0` tokens and the second one last; a deletion line is `<id> ... 0`, with one `0`,
    at least one id, and every id naming a clause not yet deleted. A new id must exceed
    every id before it, so a learned line cut short after its first `0` is refused as
    a deletion of a clause that does not exist.
    """
    nvars, clauses = parse_xnf_file(xnf_path)
    max_id = len(clauses)
    learned_count = 0
    deleted_count = 0
    line_count = 0
    refuted = False

    with open(proof_path, "r", encoding="utf-8") as f:
        for lineno, raw in enumerate(f, start=1):
            line = raw.strip()
            if not line or line.startswith("c"):
                continue
            tokens = line.split()
            line_count += 1

            try:
                if tokens[-1] != "0":
                    raise ValueError("Proof line must end with 0")
                zeros = tokens.count("0")
                if zeros == 1:
                    ids = parse_ids(tokens[:-1])
                    if not ids:
                        raise ValueError("Deletion line lists no clause id")
                    for cid in ids:
                        if cid not in clauses:
                            raise ValueError(f"Deletion of clause id {cid}, which does not exist")
                        del clauses[cid]
                    deleted_count += len(ids)
                    continue

                if zeros != 2:
                    raise ValueError("Proof line must contain exactly one or two zeros")
                idx0 = tokens.index("0")
                if idx0 == 0:
                    raise ValueError("Missing learned clause id")
                new_id = parse_ids(tokens[:1])[0]
                clause_rows = [parse_clause_token(t, nvars) for t in tokens[1:idx0]]
                reason_ids = parse_ids(tokens[idx0 + 1 : -1])

            except Exception as exc:
                print(f"ERROR line {lineno}: parse error: {exc}", file=sys.stderr)
                return 1

            if new_id <= max_id:
                print(
                    f"ERROR line {lineno}: learned id {new_id} does not exceed every earlier id ({max_id})",
                    file=sys.stderr,
                )
                return 1

            basis = Basis(nvars)
            for row in clause_rows:
                basis.add(row)

            verified = False
            for rid in reason_ids:
                if rid not in clauses:
                    print(
                        f"ERROR line {lineno}: missing reason clause id {rid} for learned id {new_id}",
                        file=sys.stderr,
                    )
                    return 1
                status, _ = propagate_clause(clauses[rid], basis, nvars)
                if status == "conflict":
                    verified = True
                    break
                if status == "deduced":
                    continue
                print(
                    f"ERROR line {lineno}: reason clause {rid} did not propagate for learned id {new_id}",
                    file=sys.stderr,
                )
                return 1

            if not verified:
                print(
                    f"ERROR line {lineno}: no conflict derived for learned id {new_id}",
                    file=sys.stderr,
                )
                return 1

            clauses[new_id] = clause_rows
            max_id = new_id
            learned_count += 1
            if not any(clause_rows):
                refuted = True

            if verbose:
                print(f"OK line {lineno}: learned {new_id}")

    if not refuted:
        print("ERROR: no line derives the empty clause", file=sys.stderr)
        return 1

    print(
        f"OK: lines={line_count} learned={learned_count} deletions={deleted_count} clauses_final={len(clauses)}"
    )
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="Check LRUP(⊕) proofs for extended DIMACS/XNF.")
    parser.add_argument("input", help="Input extended DIMACS/XNF file")
    parser.add_argument("proof", help="Proof file in LRUP(⊕) format (conventionally .lrup)")
    parser.add_argument("--verbose", action="store_true", help="Verbose per-line output")
    args = parser.parse_args()

    return check_proof(args.input, args.proof, args.verbose)


if __name__ == "__main__":
    raise SystemExit(main())
