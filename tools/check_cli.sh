#!/bin/sh
# Check the command line end to end: exit codes, error lines, a SAT model
# against its instance, and the statistics file.
#
# Usage: check_cli.sh <python> <repository root> <xorcle binary>
# Exit:  0 pass, 1 fail

set -u

python="$1"
root="$2"
xorcle="$3"

work="${TMPDIR:-/tmp}/xorcle_check_cli.$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT INT TERM

failures=0
fail() {
    echo "check_cli: $*" >&2
    failures=$((failures + 1))
}

# Run the solver, keeping stdout and stderr apart, and return its exit code.
run() {
    "$xorcle" "$@" > "$work/out" 2> "$work/err"
}

expect_status() {
    expected="$1"
    shift
    run "$@"
    status=$?
    if [ "$status" -ne "$expected" ]; then
        fail "'xorcle $*' exited $status, expected $expected"
        cat "$work/out" "$work/err" >&2
        return 1
    fi
    return 0
}

# Pass when the last run's stream $1 has a line matching $2, and fail with $3.
expect_line() {
    grep -q "$2" "$work/$1" || fail "$3"
}

if expect_status 0 --help; then
    expect_line out '^Usage: ' "--help printed no usage line"
fi
if expect_status 2 --nonsense=1 "$work/missing.xnf"; then
    expect_line err '^Error: ' "a bad option printed no Error: line"
fi
if expect_status 2 "$work/missing.xnf"; then
    expect_line err '^Error: ' "a missing file printed no Error: line"
fi

# An option that needs another: only the cyclic selector reads a miss rate.
if expect_status 2 --propagate-miss-rate=0.5 "$work/missing.xnf"; then
    expect_line err '^Error: .*--propagate-miss-rate requires --selection=cyclic' "a miss rate without the cyclic selector printed no Error: line"
fi

printf 'p xnf 3 2\n1 2 0\n' > "$work/malformed.xnf"
if expect_status 1 "$work/malformed.xnf"; then
    expect_line err '^Error: ' "a malformed instance printed no Error: line"
fi

# SAT: exit 10 and a model satisfying every clause of the instance, in which the
# unused variables 3 and 5 are false.
printf 'p xnf 6 4\n1 2 0\n-1+4 0\n2+4 6 0\n-2 -6 0\n' > "$work/sat.xnf"
if expect_status 10 "$work/sat.xnf"; then
    expect_line out '^s SATISFIABLE$' "a SAT run printed no 's SATISFIABLE'"
    expect_line out '^c Unused variables: 2$' "a SAT run did not report its two unused variables"
    "$python" - "$root/tools" "$work/sat.xnf" "$work/out" <<'EOF' || fail "the model falsifies the instance"
import sys
sys.path.insert(0, sys.argv[1])
from xnf_lib import falsified_clauses, read_rows

num_vars, clauses = read_rows(sys.argv[2])
values = [int(token) for line in open(sys.argv[3]) if line.startswith("v ")
          for token in line.split()[1:] if token != "0"]
assert sorted(abs(v) for v in values) == list(range(1, num_vars + 1)), values
assert -3 in values and -5 in values, values
assignment = sum(1 << (v - 1) for v in values if v > 0)
if falsified_clauses(clauses, assignment):
    sys.exit(f"{falsified_clauses(clauses, assignment)} clause(s) falsified by {values}")
EOF
fi

# UNSAT: exit 20.
printf 'p xnf 1 2\n1 0\n-1 0\n' > "$work/unsat.xnf"
if expect_status 20 "$work/unsat.xnf"; then
    expect_line out '^s UNSATISFIABLE$' "an UNSAT run printed no 's UNSATISFIABLE'"
fi

# --stats-file writes a header and then one row per run.
expect_status 10 --stats-file="$work/stats.csv" "$work/sat.xnf"
expect_status 20 --stats-file="$work/stats.csv" "$work/unsat.xnf"
if [ ! -s "$work/stats.csv" ]; then
    fail "--stats-file wrote nothing"
else
    lines=$(wc -l < "$work/stats.csv")
    [ "$lines" -eq 3 ] || fail "--stats-file wrote $lines lines, expected a header and two rows"
    header_columns=$(head -n 1 "$work/stats.csv" | tr ',' '\n' | wc -l)
    row_columns=$(tail -n 1 "$work/stats.csv" | tr ',' '\n' | wc -l)
    [ "$header_columns" -eq "$row_columns" ] ||
        fail "--stats-file's header has $header_columns columns and its row $row_columns"
fi

[ "$failures" -eq 0 ] || exit 1
echo "check_cli: OK"
