#!/usr/bin/env python3
"""
Shared XNF parsing and encoding utilities.
"""

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


def _parse_token(token: str, nvars: int) -> Equation:
    rhs = 1
    body = token
    if token.startswith("-"):
        rhs = 0
        body = token[1:]
        if not body:
            raise ValueError(f"Invalid token: '{token}'")
    if "-" in body:
        raise ValueError(f"Illegal '-' inside token: '{token}'")
    parts = body.split("+") if "+" in body else [body]
    vars_: List[int] = []
    for part in parts:
        if part == "":
            raise ValueError(f"Empty variable in token: '{token}'")
        v = int(part)
        if v <= 0 or v > nvars:
            raise ValueError(f"Variable out of range in token: '{token}'")
        vars_.append(v)
    if not vars_:
        raise ValueError(f"Empty equation in token: '{token}'")
    return Equation(vars=tuple(sorted(vars_)), rhs=rhs)


def parse_xnf_file(path: str) -> Tuple[int, List[Clause]]:
    with open(path, "r", encoding="utf-8") as f:
        lines = f.readlines()
    header_idx, nvars, _ = _parse_header(lines)
    tokens: List[str] = []
    for line in lines[header_idx + 1 :]:
        stripped = line.strip()
        if not stripped or stripped.startswith("c"):
            continue
        tokens.extend(stripped.split())
    clauses: List[Clause] = []
    current: List[str] = []
    for tok in tokens:
        if tok == "0":
            eqs = [_parse_token(t, nvars) for t in current]
            clauses.append(Clause(equations=eqs))
            current = []
        else:
            current.append(tok)
    if current:
        eqs = [_parse_token(t, nvars) for t in current]
        clauses.append(Clause(equations=eqs))
    return nvars, clauses


def negate_equation(eq: Equation) -> Equation:
    return Equation(vars=eq.vars, rhs=1 - eq.rhs)


def xor_compose_equations(lhs: Equation, rhs: Equation) -> Equation:
    # XOR linerals by parity over variable multiplicities and affine-constant adjustment.
    parity: Dict[int, int] = {}
    for v in lhs.vars:
        parity[v] = parity.get(v, 0) ^ 1
    for v in rhs.vars:
        parity[v] = parity.get(v, 0) ^ 1
    vars_out = sorted(v for v, bit in parity.items() if bit == 1)
    if not vars_out:
        raise ValueError("xor composition produced an empty equation")
    rhs_out = 1 ^ lhs.rhs ^ rhs.rhs
    return Equation(vars=tuple(vars_out), rhs=rhs_out)


def equation_to_token(eq: Equation) -> str:
    if not eq.vars:
        raise ValueError("cannot format empty equation")
    body = "+".join(str(v) for v in eq.vars)
    return body if eq.rhs == 1 else "-" + body


def xor_to_cnf_direct(vars_: Iterable[int], rhs: int) -> List[List[int]]:
    vars_list = list(vars_)
    k = len(vars_list)
    if k == 0:
        return [[]] if rhs == 1 else []
    if k > 24:
        raise ValueError("Direct XOR encoding only supports up to 24 variables")
    clauses: List[List[int]] = []
    for mask in range(1 << k):
        parity = 0
        clause: List[int] = []
        for i, v in enumerate(vars_list):
            bit = (mask >> i) & 1
            parity ^= bit
            clause.append(-v if bit else v)
        if parity != rhs:
            clauses.append(clause)
    return clauses


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


def encode_equation_with_output(vars_: Iterable[int], rhs: int, f_var: int, next_var: int) -> Tuple[List[List[int]], List[int], int]:
    vars_list = list(vars_)
    cnf: List[List[int]] = []
    t_vars: List[int] = []

    if len(vars_list) == 1:
        lit = vars_list[0] if rhs == 1 else -vars_list[0]
        cnf.append([-f_var, lit])
        cnf.append([f_var, -lit])
        return cnf, t_vars, next_var

    if len(vars_list) == 2:
        cnf.extend(_xor_equiv_clauses(f_var, vars_list[0], vars_list[1], rhs == 0))
        return cnf, t_vars, next_var

    prev = vars_list[0]
    for v in vars_list[1:-1]:
        t = next_var
        next_var += 1
        t_vars.append(t)
        cnf.extend(_xor_equiv_clauses(t, prev, v, False))
        prev = t
    cnf.extend(_xor_equiv_clauses(f_var, prev, vars_list[-1], rhs == 0))
    return cnf, t_vars, next_var


def split_xor_t_translation(vars_: Iterable[int], rhs: int, next_var: int) -> Tuple[List[Tuple[List[int], int]], int]:
    vars_list = list(vars_)
    if len(vars_list) <= 2:
        return [([v for v in vars_list], rhs)], next_var
    constraints: List[Tuple[List[int], int]] = []
    y_prev = next_var
    next_var += 1
    constraints.append(([vars_list[0], vars_list[1], y_prev], 0))
    for v in vars_list[2:-1]:
        y = next_var
        next_var += 1
        constraints.append(([y_prev, v, y], 0))
        y_prev = y
    constraints.append(([y_prev, vars_list[-1]], rhs))
    return constraints, next_var


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
                cnf, _, next_var = encode_equation_with_output(eq.vars, eq.rhs, f_var, next_var)
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

            yi_xor_l1 = xor_compose_equations(yi, l1)
            out.append(Clause(equations=[negate_equation(yi_xor_l1), l2]))

        out.append(Clause(equations=current))

    return next_var, out
