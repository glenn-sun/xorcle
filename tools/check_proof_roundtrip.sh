#!/bin/sh
# Solve an UNSAT instance with --proof and verify the proof with the
# independently written LRUP(⊕) checker.
#
# Usage: check_proof_roundtrip.sh [--cnf-xor] [--insert <clause line>] [--expect <regex>]
#        <repository root> <xorcle binary> <family> <params> <seed> [solver option...]
# Exit:  0 pass, 1 fail, 77 skipped (no usable python3)
#
# --cnf-xor solves and checks the instance as `tools/xnf_convert.py cnf-xor` writes it.
# --insert puts the clause line after the instance's first clause and rewrites the
# header to count every clause line, as `p xnf` unless --cnf-xor is given.
# --expect fails the run unless some line of the solver's output matches the
# extended regex, so a test can require the path it is there for.

set -eu

cnf_xor=false
insert=""
expect=""
while [ $# -gt 0 ]; do
    case "$1" in
        --cnf-xor) cnf_xor=true; shift ;;
        --insert) insert="$2"; shift 2 ;;
        --expect) expect="$2"; shift 2 ;;
        *) break ;;
    esac
done

root="$1"
xorcle="$2"
family="$3"
params="$4"
seed="$5"
shift 5

if [ -x "$root/.venv/bin/python" ]; then
    python="$root/.venv/bin/python"
elif command -v python3 >/dev/null 2>&1; then
    python="python3"
else
    echo "check_proof_roundtrip: no python3 available, skipping" >&2
    exit 77
fi

work="${TMPDIR:-/tmp}/xorcle_proof_roundtrip.$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT INT TERM

instance="$work/instance.xnf"
proof="$work/proof.lrup"

# The caller names an instance the generator writes, so none is committed.
"$python" "$root/tools/generate_xnf_tests.py" \
    --family "$family" --params "$params" --seed "$seed" --out "$instance"

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

# 20 is the solver's UNSAT exit code; anything else here is a real failure.
status=0
"$xorcle" --proof="$proof" "$@" "$instance" > "$work/solve.out" 2>&1 || status=$?
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

if [ -n "$expect" ] && ! grep -Eq "$expect" "$work/solve.out"; then
    echo "check_proof_roundtrip: no output line matches '$expect':" >&2
    cat "$work/solve.out" >&2
    exit 1
fi

if [ ! -s "$proof" ]; then
    echo "check_proof_roundtrip: solver wrote no proof to $proof" >&2
    exit 1
fi

"$python" "$root/tools/check_lrup_parity.py" "$instance" "$proof"
