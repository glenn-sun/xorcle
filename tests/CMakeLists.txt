# doctest, fetched and pinned. v2.5.3 declares `cmake_minimum_required(VERSION
# 3.14)`, which recent CMake still accepts; older doctest tags declare versions
# below 3.5 and fail to configure.
FetchContent_Declare(doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG        2d0a9359a60c51affe2a9bebb1be1dca47868151
    SYSTEM)
set(DOCTEST_WITH_TESTS OFF CACHE BOOL "" FORCE)
set(DOCTEST_NO_INSTALL ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(doctest)

add_executable(xorcle_tests
    unit/algebra_test_support.cpp
    unit/allocation_counter.cpp
    unit/heuristics_test_support.cpp
    unit/io_test_support.cpp
    unit/main.cpp
    unit/reference_matrix.cpp
    unit/test_analysis.cpp
    unit/test_attempt_budget.cpp
    unit/test_bve.cpp
    unit/test_clause_decisions.cpp
    unit/test_clause_selector.cpp
    unit/test_compaction.cpp
    unit/test_decisions.cpp
    unit/test_deletion.cpp
    unit/test_dimacs.cpp
    unit/test_ema.cpp
    unit/test_equation.cpp
    unit/test_equivalent_clause_table.cpp
    unit/test_fuzz.cpp
    unit/test_gate_solver.cpp
    unit/test_implication_graph.cpp
    unit/test_index_set.cpp
    unit/test_max_heap.cpp
    unit/test_lrup_checker.cpp
    unit/test_minimization.cpp
    unit/test_options.cpp
    unit/test_progress_reporter.cpp
    unit/test_propagation.cpp
    unit/test_random.cpp
    unit/test_restart_policy.cpp
    unit/test_row_echelon_matrix.cpp
    unit/test_saved_trail.cpp
    unit/test_solver.cpp
    unit/test_stats_writer.cpp
    unit/test_subsume_pass.cpp
    unit/test_trail.cpp
    unit/test_trail_basis.cpp)

target_link_libraries(xorcle_tests PRIVATE xorcle_core doctest::doctest)
# The LRUP(⊕) cases both checkers are held to.
target_compile_definitions(xorcle_tests PRIVATE XORCLE_TESTS_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

# The fuzz cases run as a test of their own, so `ctest -j` runs them beside
# the rest.
add_test(NAME unit COMMAND xorcle_tests --test-case-exclude=fuzz:*)
add_test(NAME fuzz COMMAND xorcle_tests --test-case=fuzz:*)

# The tests below run Python: the repository's `.venv` if there is one, else Python 3.11
# or later from the PATH. Without one they are not registered.
set(Python3_ROOT_DIR ${CMAKE_SOURCE_DIR}/.venv)
find_package(Python3 3.11 COMPONENTS Interpreter)
if(NOT Python3_Interpreter_FOUND)
    message(STATUS "No Python 3.11 or later: the tests that run Python are not registered")
    return()
endif()

# What check_cli.sh takes after its options.
set(script_args ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR} $<TARGET_FILE:xorcle>)

# Exit codes, error lines, a SAT model and the statistics file, end to end.
add_test(NAME cli
    COMMAND sh ${CMAKE_SOURCE_DIR}/tools/check_cli.sh ${script_args})

# The tests below check proofs with the checker the top level builds.
if(NOT XORCLE_PROOF_CHECKER)
    message(STATUS "No proof checker: the tests that check proofs are not registered")
    return()
endif()

# What check_proof_roundtrip.sh takes after its options.
set(roundtrip_args ${script_args} ${XORCLE_PROOF_CHECKER})

# The proof checker the roundtrips rely on, against mutated proofs.
add_test(NAME lrup_checker
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/test_check_lrup_parity.py
            ${XORCLE_PROOF_CHECKER} -v)

# Everything registers with CTest, so `ctest --test-dir build` is the single
# command. A roundtrip skips when the generator lacks a package its family needs.
add_test(NAME proof_roundtrip
    COMMAND sh ${CMAKE_SOURCE_DIR}/tools/check_proof_roundtrip.sh
            ${roundtrip_args} random_kxnf "n=14;k=3" 1)
set_tests_properties(proof_roundtrip PROPERTIES SKIP_RETURN_CODE 77)

# Minimization is on by default and rewrites the learned clause, but leaves the
# derivation a chain of resolutions, so both settings have to check.
add_test(NAME proof_roundtrip_no_minimize
    COMMAND sh ${CMAKE_SOURCE_DIR}/tools/check_proof_roundtrip.sh
            ${roundtrip_args} random_kxnf "n=14;k=3" 1
            --minimize=false)
set_tests_properties(proof_roundtrip_no_minimize PROPERTIES SKIP_RETURN_CODE 77)

# BVE and vivification run by default, so the proof without either is checked here.
add_test(NAME proof_roundtrip_no_bve_no_vivify
    COMMAND sh ${CMAKE_SOURCE_DIR}/tools/check_proof_roundtrip.sh
            ${roundtrip_args} random_kxnf "n=14;k=3" 1
            --bve=false --vivify=false)
set_tests_properties(proof_roundtrip_no_bve_no_vivify PROPERTIES SKIP_RETURN_CODE 77)

# A restart after every conflict or two changes which reasons a subsumed
# clause is still named by after the backjump, and so which replacement and
# deletion lines are written.
add_test(NAME proof_roundtrip_luby
    COMMAND sh ${CMAKE_SOURCE_DIR}/tools/check_proof_roundtrip.sh --expect-stat restarts
            ${roundtrip_args} random_kxnf "n=14;k=3" 1
            --restart=luby --restart-luby-unit=1)
set_tests_properties(proof_roundtrip_luby PROPERTIES SKIP_RETURN_CODE 77)

# The instance of proof_roundtrip_wide, whose substitutions are followed by a
# renumbering by default: here they are not, so the variables keep their numbers.
add_test(NAME proof_roundtrip_no_compact
    COMMAND sh ${CMAKE_SOURCE_DIR}/tools/check_proof_roundtrip.sh --expect-stat compactions
            ${roundtrip_args} tseitin "n=90;k=3" 1
            --compact=false)
set_tests_properties(proof_roundtrip_no_compact PROPERTIES SKIP_RETURN_CODE 77)

# BVE runs by default. Here it eliminates, each resolvent a line of the proof, and
# on the second instance it runs and eliminates nothing.
add_test(NAME proof_roundtrip_bve
    COMMAND sh ${CMAKE_SOURCE_DIR}/tools/check_proof_roundtrip.sh
            --expect-stat bve_eliminated
            ${roundtrip_args} bivium "r=531;k=51;wrong=1" 1)
set_tests_properties(proof_roundtrip_bve PROPERTIES SKIP_RETURN_CODE 77)
add_test(NAME proof_roundtrip_bve_nothing
    COMMAND sh ${CMAKE_SOURCE_DIR}/tools/check_proof_roundtrip.sh --expect-stat bve_passes
            ${roundtrip_args} random_kxnf "n=14;k=3" 1)
set_tests_properties(proof_roundtrip_bve_nothing PROPERTIES SKIP_RETURN_CODE 77)

# Here one-sided gates make equations false at the root, each a line derived from
# the gate's clauses through the lemmas of the gate search.
add_test(NAME proof_roundtrip_bve_root_equations
    COMMAND sh ${CMAKE_SOURCE_DIR}/tools/check_proof_roundtrip.sh
            --expect-stat bve_root_equations
            ${roundtrip_args} tseitin "n=90;k=3" 1)
set_tests_properties(proof_roundtrip_bve_root_equations PROPERTIES SKIP_RETURN_CODE 77)

# The default exact watchlist hands the propagator a trigger hint; the cyclic
# selector does not, so its attempts reach their reasons by a path of their own,
# and its default miss rate leaves propagation short of a fixed point.
add_test(NAME proof_roundtrip_cyclic
    COMMAND sh ${CMAKE_SOURCE_DIR}/tools/check_proof_roundtrip.sh
            ${roundtrip_args} random_kxnf "n=14;k=3" 1
            --selection=cyclic)
set_tests_properties(proof_roundtrip_cyclic PROPERTIES SKIP_RETURN_CODE 77)

# At the defaults few clauses run out of unused passes in a run this short, so this
# shortens the gap between passes and what an LBD allows until clauses that had passes
# left are deleted.
add_test(NAME proof_roundtrip_unused_passes
    COMMAND sh ${CMAKE_SOURCE_DIR}/tools/check_proof_roundtrip.sh
            ${roundtrip_args} random_kxnf "n=14;k=3" 1
            --deletion-cadence-coefficient=0.7 --deletion-cadence-offset=0 --deletion-wait=3)
set_tests_properties(proof_roundtrip_unused_passes PROPERTIES SKIP_RETURN_CODE 77)

# The sparse representation, under each selector.
add_test(NAME proof_roundtrip_sparse
    COMMAND sh ${CMAKE_SOURCE_DIR}/tools/check_proof_roundtrip.sh
            ${roundtrip_args} random_kxnf "n=14;k=3" 1
            --equation-repr=sparse)
set_tests_properties(proof_roundtrip_sparse PROPERTIES SKIP_RETURN_CODE 77)
add_test(NAME proof_roundtrip_sparse_cyclic
    COMMAND sh ${CMAKE_SOURCE_DIR}/tools/check_proof_roundtrip.sh
            ${roundtrip_args} random_kxnf "n=14;k=3" 1
            --equation-repr=sparse --selection=cyclic)
set_tests_properties(proof_roundtrip_sparse_cyclic PROPERTIES SKIP_RETURN_CODE 77)

# A rand_qp instance of 78 variables on which the implication graph feeds units,
# so the proof carries its lines; test_implication_graph checks that each of its
# four derivations writes lines that check.
add_test(NAME proof_roundtrip_implication_graph
    COMMAND sh ${CMAKE_SOURCE_DIR}/tools/check_proof_roundtrip.sh
            --expect-stat implication_graph_units_fed
            ${roundtrip_args} rand_qp "n=12;m=36;planted=0" 1)
set_tests_properties(proof_roundtrip_implication_graph PROPERTIES SKIP_RETURN_CODE 77)

# 135 variables, three words a row, so rewrite lines and the compaction records
# span word boundaries.
add_test(NAME proof_roundtrip_wide
    COMMAND sh ${CMAKE_SOURCE_DIR}/tools/check_proof_roundtrip.sh --expect-stat compactions
            ${roundtrip_args} tseitin "n=90;k=3" 1)
set_tests_properties(proof_roundtrip_wide PROPERTIES SKIP_RETURN_CODE 77)

# A clause evicted at a substitution is a deletion line; a subsumption
# replacement that a kept clause already is, is never written.
add_test(NAME proof_roundtrip_equivalent_clause_eviction
    COMMAND sh ${CMAKE_SOURCE_DIR}/tools/check_proof_roundtrip.sh
            --expect-stat equivalent_clauses
            ${roundtrip_args} ctc "r=4;b=3;k=4;wrong=1" 1)
set_tests_properties(proof_roundtrip_equivalent_clause_eviction PROPERTIES SKIP_RETURN_CODE 77)
add_test(NAME proof_roundtrip_equivalent_clause_eviction_subsumption
    COMMAND sh ${CMAKE_SOURCE_DIR}/tools/check_proof_roundtrip.sh
            --expect-stat equivalent_clauses
            ${roundtrip_args} tseitin "n=300;k=4" 1 --bve-root-equations=false)
set_tests_properties(proof_roundtrip_equivalent_clause_eviction_subsumption
                     PROPERTIES SKIP_RETURN_CODE 77)

# Vivification narrows clauses at root visits ten conflicts apart, within its
# budget; each narrowing is a line derived by the trial's propagation, and the
# clause it replaces is deleted.
add_test(NAME proof_roundtrip_vivify
    COMMAND sh ${CMAKE_SOURCE_DIR}/tools/check_proof_roundtrip.sh
            --expect-stat vivify_narrowed
            ${roundtrip_args} tseitin "n=90;k=3" 1
            --bve-root-equations=false
            --vivify-cadence=10)
set_tests_properties(proof_roundtrip_vivify PROPERTIES SKIP_RETURN_CODE 77)
# A pass due above the root forces a restart with replay, so narrowings are written
# from passes the search did not come back to the root for, and the replay
# propagates from the clauses they put in place.
add_test(NAME proof_roundtrip_vivify_forced_restart
    COMMAND sh ${CMAKE_SOURCE_DIR}/tools/check_proof_roundtrip.sh
            --expect-stat forced_restarts
            ${roundtrip_args} random_kxnf "n=14;k=3" 1
            --vivify-cadence=10)
set_tests_properties(proof_roundtrip_vivify_forced_restart PROPERTIES SKIP_RETURN_CODE 77)
add_test(NAME proof_roundtrip_phase_rotate
    COMMAND sh ${CMAKE_SOURCE_DIR}/tools/check_proof_roundtrip.sh
            --expect-stat phase_saving_decisions_recent_only
            ${roundtrip_args} random_kxnf "n=14;k=3" 1
            --phase-rotate-interval=2)
set_tests_properties(proof_roundtrip_phase_rotate PROPERTIES SKIP_RETURN_CODE 77)
# The subsumption pass, at root visits fifty conflicts apart: each strengthened
# clause is a line hinted by the wider clause and then the narrower, and a subsumed
# or strengthened clause is deleted.
add_test(NAME proof_roundtrip_subsume_pass
    COMMAND sh ${CMAKE_SOURCE_DIR}/tools/check_proof_roundtrip.sh
            --expect-stat subsumed_clauses
            ${roundtrip_args} perfect_matching "n=21;k=4" 1
            --inproc-subsume-strengthen=true --inproc-subsume-strengthen-cadence=50)
set_tests_properties(proof_roundtrip_subsume_pass PROPERTIES SKIP_RETURN_CODE 77)
add_test(NAME proof_roundtrip_stabilize
    COMMAND sh ${CMAKE_SOURCE_DIR}/tools/check_proof_roundtrip.sh
            --expect-stat focused_restarts
            ${roundtrip_args} random_kxnf "n=14;k=3" 1
            --stabilize=true --stabilize-interval=5 --focused-restart-lbd-min-ratio=1.0)
set_tests_properties(proof_roundtrip_stabilize PROPERTIES SKIP_RETURN_CODE 77)
# Half the declared variables are unused, and the proof is over all of them. The
# second instance compacts as well.
add_test(NAME proof_roundtrip_unused_vars
    COMMAND sh ${CMAKE_SOURCE_DIR}/tools/check_proof_roundtrip.sh --unused
            --expect-stat unused_vars
            ${roundtrip_args} random_kxnf "n=14;k=3" 1)
set_tests_properties(proof_roundtrip_unused_vars PROPERTIES SKIP_RETURN_CODE 77)
add_test(NAME proof_roundtrip_unused_vars_wide
    COMMAND sh ${CMAKE_SOURCE_DIR}/tools/check_proof_roundtrip.sh --unused
            --expect-stat compactions
            ${roundtrip_args} tseitin "n=90;k=3" 1)
set_tests_properties(proof_roundtrip_unused_vars_wide PROPERTIES SKIP_RETURN_CODE 77)

# CNF-XOR input numbers its XOR lines among the clauses in file order, which the
# proof's reasons cite.
add_test(NAME proof_roundtrip_cnf_xor
    COMMAND sh ${CMAKE_SOURCE_DIR}/tools/check_proof_roundtrip.sh --cnf-xor
            ${roundtrip_args} random_kxnf "n=14;k=3" 1)
set_tests_properties(proof_roundtrip_cnf_xor PROPERTIES SKIP_RETURN_CODE 77)

# An empty input clause ends the run at once, and the proof cites it by its
# position.
add_test(NAME proof_roundtrip_empty_clause
    COMMAND sh ${CMAKE_SOURCE_DIR}/tools/check_proof_roundtrip.sh --insert "0"
            ${roundtrip_args} random_kxnf "n=14;k=3" 1)
set_tests_properties(proof_roundtrip_empty_clause PROPERTIES SKIP_RETURN_CODE 77)
add_test(NAME proof_roundtrip_empty_clause_cnf_xor
    COMMAND sh ${CMAKE_SOURCE_DIR}/tools/check_proof_roundtrip.sh --cnf-xor --insert "x 0"
            ${roundtrip_args} random_kxnf "n=14;k=3" 1)
set_tests_properties(proof_roundtrip_empty_clause_cnf_xor PROPERTIES SKIP_RETURN_CODE 77)
