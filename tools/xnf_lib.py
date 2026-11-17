#!/usr/bin/env python3
"""XNF parsing and writing, and checking a model against the clauses."""

from __future__ import annotations

from typing import List, Sequence, Tuple


#: An equation as a variable mask, variable v at bit v - 1, and its right-hand side:
#: the equation holds where the mask's parity equals it.
Row = Tuple[int, int]


def variables_of(mask: int) -> List[int]:
    """The variables of a mask, ascending, 1-based."""
    out = []
    while mask:
        low = mask & -mask
        out.append(low.bit_length())
        mask ^= low
    return out


def _parse_header(lines: List[str]) -> Tuple[int, int, int]:
    for idx, line in enumerate(lines):
        stripped = line.strip()
        if not stripped or stripped.startswith("c"):
            continue
        if stripped.startswith("p"):
            parts = stripped.split()
            if len(parts) < 4 or parts[1] not in {"cnf", "xnf"}:
                raise ValueError(f"Invalid header: {stripped}")
            nvars = int(parts[2])
            nclauses = int(parts[3])
            return idx, nvars, nclauses
        raise ValueError(f"Expected header, got: {stripped}")
    raise ValueError("No header found")


def parse_token(token: str, nvars: int) -> Row:
    """One equation of a clause line, `-` for a right-hand side of 0; a repeated
    variable cancels."""
    rhs = 1
    body = token
    if token.startswith("-"):
        rhs = 0
        body = token[1:]
        if not body:
            raise ValueError(f"Invalid token: '{token}'")
    if "-" in body:
        raise ValueError(f"Illegal '-' inside token: '{token}'")
    mask = 0
    for part in body.split("+"):
        if part == "":
            raise ValueError(f"Empty variable in token: '{token}'")
        v = int(part)
        if v <= 0 or v > nvars:
            raise ValueError(f"Variable out of range in token: '{token}'")
        mask ^= 1 << (v - 1)
    return mask, rhs


def parse_xor_line(body: str, nvars: int) -> Row:
    """A CNF-XOR line `x<lit> ... 0`, its leading `x` removed: the literals XOR to 1."""
    tokens = body.split()
    if not tokens or tokens[-1] != "0" or "0" in tokens[:-1]:
        raise ValueError(f"XOR line must end with its only 0: 'x{body}'")
    mask, rhs = 0, 1
    for tok in tokens[:-1]:
        lit = int(tok)
        if lit == 0 or abs(lit) > nvars:
            raise ValueError(f"Variable out of range in XOR line: '{tok}'")
        mask ^= 1 << (abs(lit) - 1)
        rhs ^= lit < 0
    return mask, rhs


def read_rows(path: str) -> Tuple[int, List[List[Row]]]:
    """The variable count and each clause's equations, in file order; an XOR line is a
    clause of one equation."""
    with open(path, "r", encoding="utf-8") as f:
        lines = f.readlines()
    header_idx, nvars, _ = _parse_header(lines)
    clauses: List[List[Row]] = []
    current: List[Row] = []
    for line in lines[header_idx + 1 :]:
        stripped = line.strip()
        if not stripped or stripped.startswith("c"):
            continue
        if stripped.startswith("x"):
            if current:
                raise ValueError("Clause missing trailing 0 before an XOR line")
            clauses.append([parse_xor_line(stripped[1:], nvars)])
            continue
        for tok in stripped.split():
            if tok == "0":
                clauses.append(current)
                current = []
            else:
                current.append(parse_token(tok, nvars))
    if current:
        raise ValueError("Last clause missing trailing 0")
    return nvars, clauses


def write_rows(path: str, nvars: int, clauses: Sequence[Sequence[Row]]) -> None:
    """Write clauses as `p xnf`; an equation whose mask is empty cannot be written."""
    with open(path, "w", encoding="utf-8") as f:
        f.write(f"p xnf {nvars} {len(clauses)}\n")
        for clause in clauses:
            tokens = [("" if rhs else "-") + "+".join(map(str, variables_of(mask))) for mask, rhs in clause]
            f.write(" ".join(tokens + ["0"]) + "\n")


def falsified_clauses(clauses: Sequence[Sequence[Row]], model: int) -> int:
    """How many clauses `model`, variable v at bit v - 1, falsifies: none of their
    equations holds."""
    return sum(all(bin(mask & model).count("1") % 2 != rhs for mask, rhs in clause) for clause in clauses)
