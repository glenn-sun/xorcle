# Paper benchmarks

This branch contains information about the evaluation in the paper at [arXiv:2605.15002v2](https://arxiv.org/abs/2605.15002v2) corresponding to Xorcle v0.2.


## One-time setup

This benchmarking branch requires Linux x86_64. Have Python 3.11+, rustc 1.73+, CMake 3.31+, a C++23 compiler, and perf installed. For Xorricane and 2-Xornado, also have boost 1.83+, GMP development headers, autoconf, automake, and libtool installed. The following will clone this branch, set up a venv, download/build all reference solvers, and download the cryptography instances released alongside Xorricane. This will allow you to rerun the benchmarks, or a subset of the benchmarks for auditing. 

```
git clone --branch paper-benchmarks https://github.com/glenn-sun/xorcle.git && cd xorcle
cd paper-benchmarks
python3 -m venv .venv && . .venv/bin/activate && pip install -r requirements.txt
bash setup_solvers.sh 
python3 crypto/fetch.py
```

## Running a benchmark

For more information about solver configurations, see `solvers.json`. 

To run a comparison on a cryptographic instance, run the following. (Solver seeds 200-202 were fresh seeds used for the paper's cryptographic comparison, after earlier seeds were partially used to tune and validate Xorcle's heuristics.)

```
I=crypto/instances/rand_qp_type_I/tmp5h26qyoq
solvers/bin/xorcle-0.2 --seed=200 $I.xnf | grep -E '^c Conflicts|^s '
solvers/bin/kissat-4.0.4 --seed=200 $I.cnf | grep '^s '
solvers/bin/cadical-3.0.1 --seed=200 $I.cnf | grep '^s '
solvers/bin/cryptominisat5-5.17.0 --verb 1 -r 200 $I.cnf | grep '^s '
solvers/bin/xorricane-0.4.2.11 $I.xnf | grep '^s '
solvers/bin/2xornado-1.0.2 $I.2xnf | grep '^s '
```

To run a random instance, run the following. (Similarly, instance seeds 101-110 were fresh seeds used in the paper's comparison on random instances.)

```
python3 random/generate.py --lines dense_k5 --seeds 101-110
I=random/instances/scaling/dense_k5/random_kxnf_n16_k5_m366_s102.xnf
solvers/bin/xorcle-0.2 --seed=0 $I | grep -E '^c Conflicts|^s '
```

To run a proof-complexity instance, run the following.

```
python3 proof_complexity/generate.py tseitin_k3 1000
I=proof_complexity/instances/shuffle-1/tseitin/tseitin_n1000_k3_s1.cnf
solvers/bin/xorcle-0.2 --seed=0 --phase-saving=none --bve=false --vivify=false --implication-graph=false $I | grep -E '^c Conflicts|^s '
```

## Reproducing figures and tables from the paper

The following commands reproduce figures and tables from the paper, using stored JSON records `results/<experiment>/<arm>.jsonl`.

| in the paper | command | result |
|---|---|---|
| cactus plots of the 350 cryptographic instances (fig:main) | `python3 crypto/report.py` | `results/crypto/overall.png` |
| solve counts and PAR-2 for cryptographic instances | `python3 crypto/report.py` | `results/crypto/tables.md` |
| effect of each heuristic (tab:heur) | `python3 crypto/report.py` | `results/crypto/ablation.md` |
| median time against n on random formulas (fig:rand) | `python3 random/report.py` | `results/random/random.png`, `results/random/chains.md` |
| largest n solved on proof-complexity families (tab:pc) | `python3 proof_complexity/report.py` | `results/proof_complexity/largest_solved.md` |
| time and proof lines of Xorcle `--plain` (fig:pc) | `python3 proof_complexity/report.py` | `results/proof_complexity/pc.png` |

## Methodology notes

For the cryptographic comparison, we used `measure.run_one` from `measure.py`, run from this directory. It runs the following command, so that each run has 300 s and 2 GB of address space.

    perf stat -x, -e instructions:u -- timeout -k 10 300 prlimit --as=2147483648 <solver command>

We ran 32 processes at a time on a machine with two 24-core Intel Xeon Platinum 8160M chips at 2.1GHz and 754GB RAM, though the machine saw minor load from other users during this time. When reading stored JSON records of our runs, note that seed 200 ran as two concurrent pools: 28 jobs for the arms that take a seed and 4 for Xorricane and
2-Xornado, for 32 processes in all. Seed 202 for the cryptographic comparison ran only for the instances that solved in at least one of the two previous seeds for that solver, so that the median time can be accurately reported while saving compute. 

The random and proof-complexity runs used `measure.run_chains` from `measure.py`, which runs the same solver on increasingly difficult instances until timeouts occur frequently. We used `random/threshold.py` to determine the threshold for the number of clauses for a random instance to be satisfiable with 50% probability given a number of variables. These also ran at 32 processes at a time. Note that Xorcle v0.2 `--plain` was initially accidentally missed while running bijective pigeonhole formulas for $k=3$ and $k=5$ more pigeons than holes. Later, it ran as a pool of 4 jobs beside 28 other arbitrary solver processes, to have machine load comparable to other benchmarks (though the main purpose of this benchmark relates to scaling, anyway, not absolute wall time).

## Contact

For questions pertaining to benchmarking and code, contact Glenn Sun at [glennsun@cs.washington.edu](mailto:glennsun@cs.washington.edu).