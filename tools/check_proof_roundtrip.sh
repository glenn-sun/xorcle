#!/bin/sh
# Solve an UNSAT instance with --proof and verify the proof with the
# independently written LRUP(oplus) checker.
#
# This is registered with CTest so that `ctest --test-dir build` is the
# single command. It is a
# strong test precisely because tools/check_lrup_parity.py does not share any
# code with the solver.
#
# Usage: check_proof_roundtrip.sh <repository root> <xorcle binary>
# Exit:  0 pass, 1 fail, 77 skipped (no usable python3)

set -eu

root="$1"
xorcle="$2"

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

# random_kxnf_n needs no third-party generator dependency. This (family, seed)
# is one of the manifest's UNSAT instances.
"$python" "$root/tools/generate_xnf_tests.py" \
    --family random_kxnf_n --params 'n=12;k=2' --seed 4 --out "$instance"

# Compaction must be off: proof emission with compaction is not supported in
# 0.2 and the binary refuses the combination.
"$xorcle" --compact=false --proof="$proof" "$instance" > "$work/solve.out" 2>&1 || {
    echo "check_proof_roundtrip: solver failed" >&2
    cat "$work/solve.out" >&2
    exit 1
}

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
