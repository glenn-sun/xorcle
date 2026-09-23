# doctest, fetched and pinned. v2.5.3 declares `cmake_minimum_required(VERSION
# 3.14)`, which recent CMake still accepts; older doctest tags declare versions
# below 3.5 and fail to configure.
include(FetchContent)
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

add_test(NAME unit COMMAND xorcle_tests)

# Everything registers with CTest, so `ctest --test-dir build` is the single
# command. `proof_roundtrip` skips rather than fails without python3.
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

# Deletion adds deletion lines, and the checker refuses a reason it has already
# deleted, so this is where a clause deleted too early would show up.
add_test(NAME proof_roundtrip_deletion
    COMMAND sh ${CMAKE_SOURCE_DIR}/tools/check_proof_roundtrip.sh
            ${CMAKE_SOURCE_DIR} $<TARGET_FILE:xorcle> random_kxnf "n=14;k=3" 1
            --deletion=true)
set_tests_properties(proof_roundtrip_deletion PROPERTIES SKIP_RETURN_CODE 77)

# Compaction is on by default above, so this is the one variant that exercises
# a raw proof with no compaction record in it at all.
add_test(NAME proof_roundtrip_no_compact
    COMMAND sh ${CMAKE_SOURCE_DIR}/tools/check_proof_roundtrip.sh
            ${CMAKE_SOURCE_DIR} $<TARGET_FILE:xorcle> random_kxnf "n=14;k=3" 1
            --compact=false)
set_tests_properties(proof_roundtrip_no_compact PROPERTIES SKIP_RETURN_CODE 77)

# Compacting at every opportunity, so most reason clauses are proved again from
# the root units compaction threw away before a chain cites them.
add_test(NAME proof_roundtrip_compact_often
    COMMAND sh ${CMAKE_SOURCE_DIR}/tools/check_proof_roundtrip.sh
            ${CMAKE_SOURCE_DIR} $<TARGET_FILE:xorcle> random_kxnf "n=14;k=3" 1
            --compact-cadence=1)
set_tests_properties(proof_roundtrip_compact_often PROPERTIES SKIP_RETURN_CODE 77)

# The default miss rate leaves propagation short of a fixed point, so this is
# the variant that covers the derivations a complete propagation writes.
add_test(NAME proof_roundtrip_complete_propagation
    COMMAND sh ${CMAKE_SOURCE_DIR}/tools/check_proof_roundtrip.sh
            ${CMAKE_SOURCE_DIR} $<TARGET_FILE:xorcle> random_kxnf "n=14;k=3" 1
            --propagate-miss-rate=0)
set_tests_properties(proof_roundtrip_complete_propagation PROPERTIES SKIP_RETURN_CODE 77)

# The exact watchlist hands the propagator a trigger hint, which no other selector
# does, so its attempts reach their reasons by a path of their own.
add_test(NAME proof_roundtrip_exact_watchlist
    COMMAND sh ${CMAKE_SOURCE_DIR}/tools/check_proof_roundtrip.sh
            ${CMAKE_SOURCE_DIR} $<TARGET_FILE:xorcle> random_kxnf "n=14;k=3" 1
            --selection=exact_watchlist)
set_tests_properties(proof_roundtrip_exact_watchlist PROPERTIES SKIP_RETURN_CODE 77)

# An instance on which the implication graph feeds units by all four of its
# derivations: a refuted side, a side reaching one an earlier pair refuted, the
# span of the side opposite a refuted one, and the two spans of a pair with
# neither side refuted.
add_test(NAME proof_roundtrip_implication_graph
    COMMAND sh ${CMAKE_SOURCE_DIR}/tools/check_proof_roundtrip.sh
            ${CMAKE_SOURCE_DIR} $<TARGET_FILE:xorcle> rand_qp "n=15;m=30;planted=0" 1
            --implication-graph=true)
set_tests_properties(proof_roundtrip_implication_graph PROPERTIES SKIP_RETURN_CODE 77)

# The implication graph is on by default and writes its own derivations, so this
# is the variant that covers a proof with none of them in it.
add_test(NAME proof_roundtrip_no_implication_graph
    COMMAND sh ${CMAKE_SOURCE_DIR}/tools/check_proof_roundtrip.sh
            ${CMAKE_SOURCE_DIR} $<TARGET_FILE:xorcle> rand_qp "n=15;m=30;planted=0" 1
            --implication-graph=false)
set_tests_properties(proof_roundtrip_no_implication_graph PROPERTIES SKIP_RETURN_CODE 77)
