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
    unit/test_clause_decisions.cpp
    unit/test_clause_selector.cpp
    unit/test_compaction.cpp
    unit/test_decisions.cpp
    unit/test_deletion.cpp
    unit/test_dimacs.cpp
    unit/test_ema.cpp
    unit/test_equation.cpp
    unit/test_index_set.cpp
    unit/test_lrup_logger.cpp
    unit/test_max_heap.cpp
    unit/test_minimization.cpp
    unit/test_options.cpp
    unit/test_progress_reporter.cpp
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
            ${CMAKE_SOURCE_DIR} $<TARGET_FILE:xorcle>)
set_tests_properties(proof_roundtrip PROPERTIES SKIP_RETURN_CODE 77)

# Minimization is on by default and rewrites the learned clause, but leaves the
# derivation a chain of resolutions, so both settings have to check.
add_test(NAME proof_roundtrip_no_minimize
    COMMAND sh ${CMAKE_SOURCE_DIR}/tools/check_proof_roundtrip.sh
            ${CMAKE_SOURCE_DIR} $<TARGET_FILE:xorcle> --minimize=false)
set_tests_properties(proof_roundtrip_no_minimize PROPERTIES SKIP_RETURN_CODE 77)

# Deletion adds deletion lines, and the checker refuses a reason it has already
# deleted, so this is where a clause deleted too early would show up.
add_test(NAME proof_roundtrip_deletion
    COMMAND sh ${CMAKE_SOURCE_DIR}/tools/check_proof_roundtrip.sh
            ${CMAKE_SOURCE_DIR} $<TARGET_FILE:xorcle> --deletion=true)
set_tests_properties(proof_roundtrip_deletion PROPERTIES SKIP_RETURN_CODE 77)
