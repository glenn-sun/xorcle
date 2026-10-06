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
    unit/test_clause_decisions.cpp
    unit/test_clause_selector.cpp
    unit/test_compaction.cpp
    unit/test_decisions.cpp
    unit/test_deletion.cpp
    unit/test_dimacs.cpp
    unit/test_ema.cpp
    unit/test_equation.cpp
    unit/test_fuzz.cpp
    unit/test_implication_graph.cpp
    unit/test_index_set.cpp
    unit/test_max_heap.cpp
    unit/test_minimization.cpp
    unit/test_options.cpp
    unit/test_progress_reporter.cpp
    unit/test_proof_postprocess.cpp
    unit/test_proof_raw_file.cpp
    unit/test_propagation.cpp
    unit/test_random.cpp
    unit/test_restart_policy.cpp
    unit/test_row_echelon_matrix.cpp
    unit/test_solver.cpp
    unit/test_stats_writer.cpp
    unit/test_trail.cpp
    unit/test_trail_basis.cpp)

target_link_libraries(xorcle_tests PRIVATE xorcle_core doctest::doctest)

# The fuzz cases run as a test of their own, so `ctest -j` runs them beside
# the rest.
add_test(NAME unit COMMAND xorcle_tests --test-case-exclude=fuzz:*)
add_test(NAME fuzz COMMAND xorcle_tests --test-case=fuzz:*)

# Exit codes, error lines, a SAT model and the statistics file, end to end.
add_test(NAME cli
    COMMAND sh ${CMAKE_SOURCE_DIR}/tools/check_cli.sh ${CMAKE_SOURCE_DIR} $<TARGET_FILE:xorcle>)
set_tests_properties(cli PROPERTIES SKIP_RETURN_CODE 77)

# The proof checker the roundtrips rely on, against mutated proofs.
add_test(NAME lrup_checker
    COMMAND sh -c "if [ -x \"$1/.venv/bin/python\" ]; then python=\"$1/.venv/bin/python\"; \
elif command -v python3 >/dev/null 2>&1; then python=python3; else exit 77; fi; \
exec \"$python\" \"$1/tests/test_check_lrup_parity.py\" -v"
            sh ${CMAKE_SOURCE_DIR})
set_tests_properties(lrup_checker PROPERTIES SKIP_RETURN_CODE 77)

# Everything registers with CTest, so `ctest --test-dir build` is the single
# command. The roundtrips skip rather than fail without python3.
add_test(NAME proof_roundtrip
    COMMAND sh ${CMAKE_SOURCE_DIR}/tools/check_proof_roundtrip.sh
            ${CMAKE_SOURCE_DIR} $<TARGET_FILE:xorcle> random_kxnf "n=14;k=3" 1)
set_tests_properties(proof_roundtrip PROPERTIES SKIP_RETURN_CODE 77)

# Minimization is on by default and rewrites the learned clause, but leaves the
# derivation a chain of resolutions, so both settings have to check.
add_test(NAME proof_roundtrip_no_minimize
    COMMAND sh ${CMAKE_SOURCE_DIR}/tools/check_proof_roundtrip.sh
            ${CMAKE_SOURCE_DIR} $<TARGET_FILE:xorcle> random_kxnf "n=14;k=3" 1
            --minimize=false)
set_tests_properties(proof_roundtrip_no_minimize PROPERTIES SKIP_RETURN_CODE 77)

# A restart after every conflict or two changes which reasons a subsumed
# clause is still named by after the backjump, and so which replacement and
# deletion lines are written.
add_test(NAME proof_roundtrip_luby
    COMMAND sh ${CMAKE_SOURCE_DIR}/tools/check_proof_roundtrip.sh --expect "^c Restarts: [1-9]"
            ${CMAKE_SOURCE_DIR} $<TARGET_FILE:xorcle> random_kxnf "n=14;k=3" 1
            --restart=luby --restart-luby-unit=1)
set_tests_properties(proof_roundtrip_luby PROPERTIES SKIP_RETURN_CODE 77)

# Compaction is on by default above, so this is the one variant that exercises
# a raw proof with no compaction record in it at all, and, since the implication
# graph needs compaction, none of the graph's lines either.
add_test(NAME proof_roundtrip_no_compact
    COMMAND sh ${CMAKE_SOURCE_DIR}/tools/check_proof_roundtrip.sh
            ${CMAKE_SOURCE_DIR} $<TARGET_FILE:xorcle> random_kxnf "n=14;k=3" 1
            --compact=false)
set_tests_properties(proof_roundtrip_no_compact PROPERTIES SKIP_RETURN_CODE 77)

# The default exact watchlist hands the propagator a trigger hint; the cyclic
# selector does not, so its attempts reach their reasons by a path of their own,
# and its default miss rate leaves propagation short of a fixed point.
add_test(NAME proof_roundtrip_cyclic
    COMMAND sh ${CMAKE_SOURCE_DIR}/tools/check_proof_roundtrip.sh
            ${CMAKE_SOURCE_DIR} $<TARGET_FILE:xorcle> random_kxnf "n=14;k=3" 1
            --selection=cyclic)
set_tests_properties(proof_roundtrip_cyclic PROPERTIES SKIP_RETURN_CODE 77)

# The sparse representation, which runs only the cyclic selector.
add_test(NAME proof_roundtrip_sparse
    COMMAND sh ${CMAKE_SOURCE_DIR}/tools/check_proof_roundtrip.sh
            ${CMAKE_SOURCE_DIR} $<TARGET_FILE:xorcle> random_kxnf "n=14;k=3" 1
            --equation-repr=sparse --selection=cyclic)
set_tests_properties(proof_roundtrip_sparse PROPERTIES SKIP_RETURN_CODE 77)

# A rand_qp instance of 78 variables on which the implication graph feeds units,
# so the proof carries its lines; test_implication_graph checks that each of its
# four derivations writes lines that check.
add_test(NAME proof_roundtrip_implication_graph
    COMMAND sh ${CMAKE_SOURCE_DIR}/tools/check_proof_roundtrip.sh
            --expect "^c Units fed to the root trail: [1-9]"
            ${CMAKE_SOURCE_DIR} $<TARGET_FILE:xorcle> rand_qp "n=12;m=36;planted=0" 1)
set_tests_properties(proof_roundtrip_implication_graph PROPERTIES SKIP_RETURN_CODE 77)

# 135 variables, three words a row, so rewrite lines and the compaction records
# span word boundaries.
add_test(NAME proof_roundtrip_wide
    COMMAND sh ${CMAKE_SOURCE_DIR}/tools/check_proof_roundtrip.sh --expect "^c Compactions: [1-9]"
            ${CMAKE_SOURCE_DIR} $<TARGET_FILE:xorcle> tseitin "n=90;k=3" 1)
set_tests_properties(proof_roundtrip_wide PROPERTIES SKIP_RETURN_CODE 77)

# CNF-XOR input numbers its XOR lines among the clauses in file order, which the
# proof's reasons cite.
add_test(NAME proof_roundtrip_cnf_xor
    COMMAND sh ${CMAKE_SOURCE_DIR}/tools/check_proof_roundtrip.sh --cnf-xor
            ${CMAKE_SOURCE_DIR} $<TARGET_FILE:xorcle> random_kxnf "n=14;k=3" 1)
set_tests_properties(proof_roundtrip_cnf_xor PROPERTIES SKIP_RETURN_CODE 77)

# An empty input clause ends the run at once, and the proof cites it by its
# position.
add_test(NAME proof_roundtrip_empty_clause
    COMMAND sh ${CMAKE_SOURCE_DIR}/tools/check_proof_roundtrip.sh --insert "0"
            ${CMAKE_SOURCE_DIR} $<TARGET_FILE:xorcle> random_kxnf "n=14;k=3" 1)
set_tests_properties(proof_roundtrip_empty_clause PROPERTIES SKIP_RETURN_CODE 77)
add_test(NAME proof_roundtrip_empty_clause_cnf_xor
    COMMAND sh ${CMAKE_SOURCE_DIR}/tools/check_proof_roundtrip.sh --cnf-xor --insert "x 0"
            ${CMAKE_SOURCE_DIR} $<TARGET_FILE:xorcle> random_kxnf "n=14;k=3" 1)
set_tests_properties(proof_roundtrip_empty_clause_cnf_xor PROPERTIES SKIP_RETURN_CODE 77)
