#!/usr/bin/env bash
# The whole pipeline, in the order the paper's results were produced. Each step resumes
# where it stopped, so a rerun with the committed results in place measures nothing and
# only rewrites the tables and figures. Measuring everything takes several days at 32 jobs.
#
#   run_all.sh            from this directory, with the venv of requirements.txt active,
#                         perf on PATH, and Xorcle built at ../build (solvers.json)
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
PY=${PYTHON:-python3}

bash setup_solvers.sh

# Danner's cryptographic instances: seed 200 as two concurrent pools (28 jobs for the arms
# that take a seed, 4 for Xorricane and 2-Xornado), then seed 201, then seed 202 on the
# pairs solved at seed 200 or 201.
$PY crypto/fetch.py
$PY crypto/run.py --seed 200 --arms seeded --jobs 28 &
seeded=$!
$PY crypto/run.py --seed 200 --arms unseeded --jobs 4 &
unseeded=$!
wait "$seeded"
wait "$unseeded"
$PY crypto/run.py --seed 201 --arms seeded --jobs 32
$PY crypto/run.py --seed 202 --arms seeded --solved-at 200,201 --jobs 32
$PY crypto/run.py --check-models
$PY crypto/report.py

# Random XNF: the threshold sweep, then the scaling runs at the fitted m*(n).
$PY random/threshold.py probe --sparse-ratio 0.9 --jobs 8
$PY random/threshold.py run --jobs 32
$PY random/threshold.py fit
$PY random/generate.py
$PY random/run.py --jobs 32
$PY random/run.py --certify-unsat --jobs 32
$PY random/threshold.py certify --jobs 32
$PY random/run.py --check-models
$PY random/report.py

# Proof complexity: every chain, with a checked proof for every point Xorcle solves.
$PY proof_complexity/run.py --jobs 32
$PY proof_complexity/report.py
