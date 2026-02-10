#!/usr/bin/env python3
"""
convert_plus_to_dimacs.py

Convert a DIMACS-plus file (tokens like "1+2 -3 0", where leading '-' means RHS=0)
into a standard DIMACS CNF file using Tseitin definitions for multi-variable XOR
(parity) equations.

Encoding details:
 - A token "a+b+c" means x_a ⊕ x_b ⊕ x_c = 1 (no leading '-') or = 0 (leading '-').
 - Single-variable tokens "k" or "-k" are mapped directly to the variable or its negation.
 - For k >= 2, we create a chain of aux variables:
       aux1 = v1 ⊕ v2
       aux2 = aux1 ⊕ v3
       ...
       aux_final = aux_{k-1} ⊕ v_k
   Each binary XOR x ↔ (y ⊕ z) is expressed by the 4 CNF clauses:
       (¬x ∨ y ∨ z)
       (¬x ∨ ¬y ∨ ¬z)
       ( x ∨ y ∨ ¬z)
       ( x ∨ ¬y ∨ z)
 - The equation token is replaced by literal (aux_final) if RHS=1, or (¬aux_final) if RHS=0.
 - Each original clause (disjunction of equation-tokens) becomes a standard CNF clause over
   original variables and the introduced aux variables.

The script supports files where clauses can span lines (we parse tokens until '0' as clause terminator).
"""

import sys
from typing import List, Tuple

def parse_plus_dimacs_tokens(filepath: str) -> Tuple[int, List[List[str]]]:
    """
    Parse the input file and return (n_vars, clauses_tokens),
    where clauses_tokens is a list of clauses and each clause is a list of token strings.
    Token strings are exactly the whitespace tokens appearing in the file (e.g. "1+2", "-3", "0").
    """
    with open(filepath, "r") as f:
        lines = f.readlines()

    # remove leading/trailing whitespace
    lines = [ln.strip() for ln in lines]

    # find header line starting with 'p'
    header_idx = None
    for i, ln in enumerate(lines):
        if ln.startswith('p '):
            header_idx = i
            break
    if header_idx is None:
        raise RuntimeError("No 'p cnf n m' header line found")

    header_parts = lines[header_idx].split()
    if len(header_parts) < 4 or header_parts[1] != 'cnf':
        raise RuntimeError("Invalid header: " + lines[header_idx])
    try:
        n_vars = int(header_parts[2])
    except Exception as e:
        raise RuntimeError("Cannot parse number of variables from header: " + lines[header_idx]) from e

    # collect tokens after header, skipping comment lines that begin with 'c'
    tokens = []
    for ln in lines[header_idx+1:]:
        if not ln:
            continue
        if ln.startswith('c'):
            continue
        parts = ln.split()
        # keep tokens as-is
        tokens.extend(parts)

    # group tokens into clauses by locating '0' terminators
    clauses_tokens = []
    cur = []
    for tok in tokens:
        if tok == 'c':
            # uncommon: standalone 'c' token; just skip to be safe
            continue
        if tok == '0':
            if not cur:
                # empty clause? We'll still append an empty list to indicate an empty clause
                clauses_tokens.append([])
            else:
                clauses_tokens.append(cur)
            cur = []
        else:
            cur.append(tok)
    if cur:
        # If file doesn't end with 0 but leftover tokens exist, we treat them as a clause
        clauses_tokens.append(cur)

    return n_vars, clauses_tokens

def encode_xor_def(aux: int, y: int, z: int) -> List[List[int]]:
    """
    Return CNF clauses (as lists of ints) encoding aux <-> (y XOR z).
    Using the 4-clause standard encoding:
      (¬aux ∨ y ∨ z)
      (¬aux ∨ ¬y ∨ ¬z)
      ( aux ∨ y ∨ ¬z)
      ( aux ∨ ¬y ∨ z)
    Literals are integers where negative means negation.
    """
    return [
        [-aux,  y,  z],
        [-aux, -y, -z],
        [ aux,  y, -z],
        [ aux, -y,  z],
    ]

def convert_plus_to_standard(n_vars: int, clauses_tokens: List[List[str]]):
    """
    Convert tokenized clauses to standard CNF clauses and produce final var count and clause list.
    Returns (final_num_vars, final_clause_list), where final_clause_list is a list of clauses,
    each clause is a list of integer literals in DIMACS format.
    """
    clauses_out: List[List[int]] = []
    next_aux = n_vars  # auxiliary variables will be assigned as next_aux+1, next_aux+2, ...
    
    for clause_idx, clause_tokens in enumerate(clauses_tokens):
        # list of literals (int) that will form the translated clause
        clause_literals: List[int] = []

        for token in clause_tokens:
            token = token.strip()
            if token == '':
                continue
            # single-literal token (no '+') or multi-variable
            if '+' not in token:
                # token should be an integer like '3' or '-3'
                try:
                    lit = int(token)
                except Exception as e:
                    raise RuntimeError(f"Invalid token '{token}' in clause {clause_idx}") from e
                if lit == 0:
                    # should not happen here (0 should have terminated clause), but skip
                    continue
                # In the plus-format, a single integer literal 'k' denotes equation x_k = 1,
                # and '-k' denotes x_k = 0. So we can directly use literal k or -k in resulting clause.
                clause_literals.append(lit)
            else:
                # multi-variable XOR token, possibly with leading '-'
                rhs_zero = False
                body = token
                if token[0] == '-':
                    rhs_zero = True
                    body = token[1:]
                # ensure no '-' in body
                if '-' in body:
                    raise RuntimeError(f"Illegal '-' inside + expression: '{token}' in clause {clause_idx}")
                parts = body.split('+')
                if any(p == '' for p in parts):
                    raise RuntimeError(f"Empty variable in token '{token}' in clause {clause_idx}")
                try:
                    vars_list = [int(p) for p in parts]
                except Exception as e:
                    raise RuntimeError(f"Non-integer in token '{token}' in clause {clause_idx}") from e
                # validate variable indices
                for v in vars_list:
                    if v < 1 or v > n_vars:
                        raise RuntimeError(f"Variable index {v} out of range 1..{n_vars} in token '{token}'")

                k = len(vars_list)
                if k == 1:
                    # single variable case, handled same as above but honor rhs_zero
                    v = vars_list[0]
                    lit = -v if rhs_zero else v
                    clause_literals.append(lit)
                else:
                    # Build a chain of XOR defs:
                    # aux1 = v1 XOR v2
                    # aux2 = aux1 XOR v3
                    # ...
                    # final_aux = aux_{k-1} XOR vk
                    prev = None
                    for i in range(k):
                        if i == 0:
                            # start with v1, but can't define XOR without v2; so skip until i==1
                            prev = vars_list[0]
                            continue
                        cur_var = vars_list[i]
                        # create new auxiliary variable
                        next_aux += 1
                        aux_var = next_aux
                        # encode aux_var <-> (prev XOR cur_var)
                        # prev might be an original variable (positive int) or previous aux (positive int)
                        # we treat them both as positive var numbers
                        clauses_out.extend(encode_xor_def(aux_var, prev, cur_var))
                        prev = aux_var
                    final_aux = prev  # the last aux variable created
                    # If RHS is 1 token meant XOR(vars)=1 -> equation is true iff final_aux == 1
                    # If RHS is 0 token meant XOR(vars)=0 -> equation is true iff final_aux == 0 -> use negation
                    eq_lit = -final_aux if rhs_zero else final_aux
                    clause_literals.append(eq_lit)

        # Now we have the translated literals for this clause (may be empty -> empty clause)
        # Append as a CNF clause. Empty clause represents false (unsatisfiable).
        clauses_out.append(clause_literals)

    final_num_vars = next_aux
    return final_num_vars, clauses_out

def write_dimacs(filepath: str, num_vars: int, clauses: List[List[int]], comment: str = None):
    """
    Write clauses to a DIMACS file.
    clauses is a list of lists of ints; each inner list is a clause; '0' terminates each clause line.
    """
    num_clauses = len(clauses)
    with open(filepath, "w") as f:
        if comment:
            for line in comment.splitlines():
                f.write(f"c {line}\n")
        f.write(f"p cnf {num_vars} {num_clauses}\n")
        for clause in clauses:
            if clause:
                f.write(" ".join(str(l) for l in clause) + " 0\n")
            else:
                # empty clause
                f.write("0\n")

def main():
    if len(sys.argv) != 3:
        print("Usage: python3 convert_plus_to_dimacs.py input_plus.cnf output.cnf")
        sys.exit(2)
    src = sys.argv[1]
    dst = sys.argv[2]

    n_vars, clauses_tokens = parse_plus_dimacs_tokens(src)
    final_vars, final_clauses = convert_plus_to_standard(n_vars, clauses_tokens)
    comment = ("Converted from DIMACS-plus (tokens like '1+2 -3 0') to standard DIMACS "
               "using Tseitin XOR chaining. Each multi-variable token uses k-1 aux vars and 4(k-1) clauses.")
    write_dimacs(dst, final_vars, final_clauses, comment)
    print(f"Wrote {len(final_clauses)} clauses with {final_vars} variables to {dst}")

if __name__ == "__main__":
    main()
