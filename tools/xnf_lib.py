#!/usr/bin/env python3
"""Shared XNF parsing, formatting, and conversion helpers."""

from __future__ import annotations

import itertools
from dataclasses import dataclass
from typing import Dict, Iterable, List, Sequence, Tuple


@dataclass(frozen=True)
class Equation:
    vars: Tuple[int, ...]  # 1-based variable indices, sorted
    rhs: int               # 0 or 1


@dataclass
class Clause:
    equations: List[Equation]


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


def parse_xnf_file(path: str) -> Tuple[int, List[Clause]]:
    nvars, rows = read_rows(path)
    clauses = [
        Clause(equations=[Equation(vars=tuple(variables_of(mask)), rhs=rhs) for mask, rhs in clause])
        for clause in rows
    ]
    return nvars, clauses


def negate_equation(eq: Equation) -> Equation:
    return Equation(vars=eq.vars, rhs=1 - eq.rhs)


def negation_of_sum(lhs: Equation, rhs: Equation) -> Equation:
    """The negation of `lhs + rhs`: their variables' sum, equal to `1 + lhs.rhs + rhs.rhs`."""
    parity: Dict[int, int] = {}
    for v in lhs.vars:
        parity[v] = parity.get(v, 0) ^ 1
    for v in rhs.vars:
        parity[v] = parity.get(v, 0) ^ 1
    vars_out = sorted(v for v, bit in parity.items() if bit == 1)
    if not vars_out:
        raise ValueError("equation sum is empty")
    rhs_out = 1 ^ lhs.rhs ^ rhs.rhs
    return Equation(vars=tuple(vars_out), rhs=rhs_out)


def equation_to_token(eq: Equation) -> str:
    if not eq.vars:
        raise ValueError("cannot format empty equation")
    body = "+".join(str(v) for v in eq.vars)
    return body if eq.rhs == 1 else "-" + body


def _xor_equiv_clauses(z: int, x: int, y: int, negate: bool) -> List[List[int]]:
    if not negate:
        return [
            [-z,  x,  y],
            [-z, -x, -y],
            [ z,  x, -y],
            [ z, -x,  y],
        ]
    return [
        [-z,  x, -y],
        [-z, -x,  y],
        [ z,  x,  y],
        [ z, -x, -y],
    ]


def encode_equation_with_output(vars_: Iterable[int], rhs: int, f_var: int, next_var: int) -> Tuple[List[List[int]], int]:
    vars_list = list(vars_)
    cnf: List[List[int]] = []

    if len(vars_list) == 1:
        lit = vars_list[0] if rhs == 1 else -vars_list[0]
        cnf.append([-f_var, lit])
        cnf.append([f_var, -lit])
        return cnf, next_var

    if len(vars_list) == 2:
        cnf.extend(_xor_equiv_clauses(f_var, vars_list[0], vars_list[1], rhs == 0))
        return cnf, next_var

    prev = vars_list[0]
    for v in vars_list[1:-1]:
        t = next_var
        next_var += 1
        cnf.extend(_xor_equiv_clauses(t, prev, v, False))
        prev = t
    cnf.extend(_xor_equiv_clauses(f_var, prev, vars_list[-1], rhs == 0))
    return cnf, next_var


def convert_xnf_to_cnf(nvars: int, clauses: List[Clause]) -> Tuple[List[List[int]], Dict[Equation, int], int]:
    cnf_clauses: List[List[int]] = []
    eq_to_f: Dict[Equation, int] = {}
    next_var = nvars + 1

    for clause in clauses:
        f_vars: List[int] = []
        for eq in clause.equations:
            if eq not in eq_to_f:
                f_var = next_var
                next_var += 1
                eq_to_f[eq] = f_var
                cnf, next_var = encode_equation_with_output(eq.vars, eq.rhs, f_var, next_var)
                cnf_clauses.extend(cnf)
            f_vars.append(eq_to_f[eq])
        cnf_clauses.append(f_vars)

    return cnf_clauses, eq_to_f, next_var - 1


def evaluate_equation(eq: Equation, assignment_map: Dict[int, int]) -> bool:
    parity = 0
    for v in eq.vars:
        parity ^= assignment_map.get(v, 0)
    return parity == eq.rhs


def clause_support_vars(clause: Clause) -> List[int]:
    support = set()
    for eq in clause.equations:
        support.update(eq.vars)
    return sorted(support)


def encode_xnf_clause_to_cnf_naive(clause: Clause) -> List[List[int]]:
    support = clause_support_vars(clause)
    out: List[List[int]] = []
    for bits in itertools.product((0, 1), repeat=len(support)):
        assignment = {v: bit for v, bit in zip(support, bits)}
        if any(evaluate_equation(eq, assignment) for eq in clause.equations):
            continue
        out.append([v if assignment[v] == 0 else -v for v in support])
    return out


def convert_xnf_to_cnf_naive(nvars: int, clauses: Sequence[Clause]) -> List[List[int]]:
    del nvars  # Header var count is preserved by caller; conversion uses no fresh variables.
    cnf_clauses: List[List[int]] = []
    for clause in clauses:
        cnf_clauses.extend(encode_xnf_clause_to_cnf_naive(clause))
    return cnf_clauses


def convert_xnf_to_cnf_xor(
    nvars: int, clauses: List[Clause]
) -> Tuple[List[List[int]], List[List[int]], Dict[Equation, int], int]:
    """
    Convert XNF to CNF-XOR using a single fresh output variable per equation.

    Returns (cnf_clauses, xor_clauses, eq_to_f, total_vars).
    cnf_clauses contain only the disjunctions of output variables for each clause.
    xor_clauses contain one XOR definition per unique equation.
    """
    cnf_clauses: List[List[int]] = []
    xor_clauses: List[List[int]] = []
    eq_to_f: Dict[Equation, int] = {}
    next_var = nvars + 1

    for clause in clauses:
        f_vars: List[int] = []
        for eq in clause.equations:
            if eq not in eq_to_f:
                f_var = next_var
                next_var += 1
                eq_to_f[eq] = f_var
                xor_line = [f_var] + list(eq.vars)
                if eq.rhs == 1:
                    xor_line[0] = -xor_line[0]
                xor_clauses.append(xor_line)
            f_vars.append(eq_to_f[eq])
        cnf_clauses.append(f_vars)

    return cnf_clauses, xor_clauses, eq_to_f, next_var - 1


def write_cnf(path: str, nvars: int, clauses: List[List[int]]) -> None:
    with open(path, "w", encoding="utf-8") as f:
        f.write(f"p cnf {nvars} {len(clauses)}\n")
        for clause in clauses:
            if clause:
                f.write(" ".join(str(l) for l in clause) + " 0\n")
            else:
                f.write("0\n")


def write_xnf(path: str, nvars: int, clauses: List[Clause]) -> None:
    with open(path, "w", encoding="utf-8") as f:
        f.write(f"p cnf {nvars} {len(clauses)}\n")
        for clause in clauses:
            if clause.equations:
                f.write(" ".join(equation_to_token(eq) for eq in clause.equations) + " 0\n")
            else:
                f.write("0\n")


def convert_xnf_to_2xnf(nvars: int, clauses: List[Clause]) -> Tuple[int, List[Clause]]:
    next_var = nvars
    out: List[Clause] = []

    for clause in clauses:
        current = list(clause.equations)
        while len(current) > 2:
            l1 = current[0]
            l2 = current[1]
            rest = current[2:]

            next_var += 1
            yi = Equation(vars=(next_var,), rhs=1)

            current = [yi] + rest
            out.append(Clause(equations=[yi, negate_equation(l2)]))

            yi_plus_l1 = negate_equation(negation_of_sum(yi, l1))
            out.append(Clause(equations=[yi_plus_l1, l2]))

        out.append(Clause(equations=current))

    return next_var, out
