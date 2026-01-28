#!/usr/bin/env python3

# This code was written with the assistance of generative AI.

"""
filter_cnf_by_vars.py

Recursively scan an input directory for DIMACS CNF files and copy any CNF file
with <= max_vars variables to the output directory, preserving relative paths.

Defaults:
    input_dir  = "submitted"
    output_dir = "filtered"
    max_vars   = 100
"""
import argparse
import os
import re
import shutil
from pathlib import Path

INT_RE = re.compile(r'-?\d+')

def count_vars_in_dimacs(path):
    """
    Try to determine the number of variables in a DIMACS CNF file.
    1) First look for a 'p cnf <num_vars> <num_clauses>' header.
    2) If no header, scan the file for integer tokens (clause literals) and
       return the maximum absolute variable index found.
    Returns:
        int number_of_variables OR None if file doesn't look like a CNF.
    """
    max_var = 0
    saw_lit_like_line = False

    try:
        with open(path, 'r', errors='ignore') as f:
            for line in f:
                s = line.strip()
                if not s:
                    continue
                # skip comment lines
                if s.startswith('c') or s.startswith('%') or s.startswith('0'):
                    continue
                # header line
                if s.startswith('p'):
                    parts = s.split()
                    # Expect something like: p cnf <vars> <clauses>
                    if len(parts) >= 3 and parts[1].lower() == 'cnf':
                        try:
                            num_vars = int(parts[2])
                            return num_vars
                        except ValueError:
                            pass
                # otherwise try to find integers in the line (clause)
                found = INT_RE.findall(s)
                if found:
                    # treat as clause-like line if it contains at least one non-zero int
                    for tok in found:
                        v = int(tok)
                        if v != 0:
                            saw_lit_like_line = True
                            if abs(v) > max_var:
                                max_var = abs(v)
    except (IOError, OSError):
        return None

    if saw_lit_like_line and max_var > 0:
        return max_var

    # no evidence this is a CNF file
    return None

def copy_if_small_vars(in_path, in_root, out_root, max_vars):
    num_vars = count_vars_in_dimacs(in_path)
    if num_vars is None:
        return False, None
    if num_vars <= max_vars:
        # preserve relative path
        rel = os.path.relpath(in_path, in_root)
        dest = os.path.join(out_root, rel)
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        shutil.copy2(in_path, dest)
        return True, num_vars
    return False, num_vars

def main():
    p = argparse.ArgumentParser(description="Copy DIMACS CNF files with <= N variables into output dir.")
    p.add_argument("input_dir", nargs="?", default="submitted", help="Input directory to scan (default: submitted)")
    p.add_argument("output_dir", nargs="?", default="filtered", help="Output directory (default: filtered)")
    p.add_argument("--max-vars", type=int, default=100, help="Maximum number of variables to keep (default: 100)")
    args = p.parse_args()

    input_dir = os.path.abspath(args.input_dir)
    output_dir = os.path.abspath(args.output_dir)
    max_vars = args.max_vars

    if not os.path.isdir(input_dir):
        print(f"Input directory does not exist or is not a directory: {input_dir}")
        return

    os.makedirs(output_dir, exist_ok=True)

    total_files = 0
    cnf_files = 0
    copied = 0

    for root, _, files in os.walk(input_dir):
        for fn in files:
            total_files += 1
            full = os.path.join(root, fn)
            try:
                kept, num_vars = copy_if_small_vars(full, input_dir, output_dir, max_vars)
            except Exception as e:
                # Skip unreadable files but show a message
                print(f"Warning: failed to process {full}: {e}")
                continue

            if num_vars is not None:
                cnf_files += 1
                if kept:
                    copied += 1
                    print(f"Copied: {os.path.relpath(full, input_dir)}  (vars={num_vars})")

    print()
    print(f"Scanned files: {total_files}")
    print(f"Detected CNF-like files: {cnf_files}")
    print(f"Copied (<= {max_vars} vars): {copied}")
    print(f"Output root: {output_dir}")

if __name__ == "__main__":
    main()
