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

# The proof checker proof_check relies on, against mutated proofs.
add_test(NAME lrup_checker
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/test_check_lrup_parity.py
            ${XORCLE_PROOF_CHECKER} -v)

# Every row of proof_checks.csv: an UNSAT instance from instances/ solved with --proof
# under the row's options, the proof checked, and the row's statistics required above
# zero, so each row pins the path it is there for. Every failing row is reported.
add_test(NAME proof_check
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/run_proof_checks.py
            ${CMAKE_SOURCE_DIR} $<TARGET_FILE:xorcle> ${XORCLE_PROOF_CHECKER})
