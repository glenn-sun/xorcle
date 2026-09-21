#!/bin/sh
# Solve an UNSAT instance with --proof and verify the proof with the
# independently written LRUP(⊕) checker.
#
# Usage: check_proof_roundtrip.sh <repository root> <xorcle binary> <family> <params>
#        <seed> [solver option...]
# Exit:  0 pass, 1 fail, 77 skipped (no usable python3)

set -eu

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

if [ ! -s "$proof" ]; then
    echo "check_proof_roundtrip: solver wrote no proof to $proof" >&2
    exit 1
fi

"$python" "$root/tools/check_lrup_parity.py" "$instance" "$proof"
