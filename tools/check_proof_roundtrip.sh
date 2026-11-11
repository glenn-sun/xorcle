#!/bin/sh
# Solve an UNSAT instance with --proof and verify the proof with the
# independently written LRUP(⊕) checker.
#
# Usage: check_proof_roundtrip.sh [--cnf-xor] [--insert <clause line>] [--unused]
#        [--expect-stat <column>]
#        <python> <repository root> <xorcle binary> <family> <params> <seed> [solver option...]
# Exit:  0 pass, 1 fail, 77 skipped (the generator lacks a dependency)
#
# --cnf-xor solves and checks the instance as `tools/xnf_convert.py cnf-xor` writes it.
# --insert puts the clause line after the instance's first clause and rewrites the
# header to count every clause line, as `p xnf` unless --cnf-xor is given.
# --unused doubles every variable's number and the header's count, so that the odd
# variables are unused.
# --expect-stat fails the run unless the solver's statistics put the named CSV column
# above zero, so a test can require the path it is there for.

set -eu

cnf_xor=false
insert=""
unused=false
expect_stat=""
while [ $# -gt 0 ]; do
    case "$1" in
        --cnf-xor) cnf_xor=true; shift ;;
        --insert) insert="$2"; shift 2 ;;
        --unused) unused=true; shift ;;
        --expect-stat) expect_stat="$2"; shift 2 ;;
        *) break ;;
    esac
done

python="$1"
root="$2"
xorcle="$3"
family="$4"
params="$5"
seed="$6"
shift 6

work="${TMPDIR:-/tmp}/xorcle_proof_roundtrip.$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT INT TERM

instance="$work/instance.xnf"
proof="$work/proof.lrup"

# The caller names an instance the generator writes, so none is committed. The
# generator exits 3 when the family needs a package that is not installed.
status=0
"$python" "$root/tools/generate_xnf_tests.py" \
    --family "$family" --params "$params" --seed "$seed" --out "$instance" || status=$?
if [ "$status" -eq 3 ]; then
    echo "check_proof_roundtrip: the generator lacks a dependency for $family, skipping" >&2
    exit 77
fi
if [ "$status" -ne 0 ]; then
    exit "$status"
fi

if [ "$cnf_xor" = true ]; then
    "$python" "$root/tools/xnf_convert.py" cnf-xor "$instance" "$work/instance.cnf"
    instance="$work/instance.cnf"
fi

if [ -n "$insert" ]; then
    format=xnf
    if [ "$cnf_xor" = true ]; then
        format=cnf
    fi
    awk -v line="$insert" -v format="$format" '
        /^[ \t]*(c|$)/ { next }
        /^[ \t]*p/ { num_vars = $3; next }
        { clauses[++n] = $0; if (n == 1) clauses[++n] = line }
        END {
            print "p", format, num_vars, n
            for (i = 1; i <= n; ++i) print clauses[i]
        }' "$instance" > "$work/inserted"
    instance="$work/inserted"
fi

if [ "$unused" = true ]; then
    awk '
        /^[ \t]*c/ { print; next }
        /^[ \t]*p/ { print $1, $2, 2 * $3, $4; next }
        {
            rest = $0
            doubled = ""
            while (match(rest, /[0-9]+/)) {
                doubled = doubled substr(rest, 1, RSTART - 1) (2 * substr(rest, RSTART, RLENGTH))
                rest = substr(rest, RSTART + RLENGTH)
            }
            print doubled rest
        }' "$instance" > "$work/unused"
    instance="$work/unused"
fi

# 20 is the solver's UNSAT exit code; anything else here is a real failure.
status=0
"$xorcle" --proof="$proof" ${expect_stat:+"--stats-file=$work/stats.csv"} "$@" "$instance" \
    > "$work/solve.out" 2>&1 || status=$?
if [ "$status" -ne 20 ]; then
    echo "check_proof_roundtrip: solver exited $status, expected 20 (UNSAT)" >&2
    cat "$work/solve.out" >&2
    exit 1
fi

if ! grep -q '^s UNSATISFIABLE' "$work/solve.out"; then
    echo "check_proof_roundtrip: expected UNSAT, got:" >&2
    cat "$work/solve.out" >&2
    exit 1
fi

if [ -n "$expect_stat" ]; then
    "$python" -c '
import csv, sys
row = next(csv.DictReader(open(sys.argv[1])))
column = sys.argv[2]
if column not in row:
    sys.exit(f"check_proof_roundtrip: the statistics have no column {column}")
if not float(row[column]) > 0:
    sys.exit(f"check_proof_roundtrip: {column} is {row[column]}, expected above zero")
' "$work/stats.csv" "$expect_stat"
fi

if [ ! -s "$proof" ]; then
    echo "check_proof_roundtrip: solver wrote no proof to $proof" >&2
    exit 1
fi

"$python" "$root/tools/check_lrup_parity.py" "$instance" "$proof"
