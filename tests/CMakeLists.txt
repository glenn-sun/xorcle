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
    unit/test_bitmap.cpp
    unit/test_clause_selector.cpp
    unit/test_compaction.cpp
    unit/test_decision_utils.cpp
    unit/test_decisions.cpp
    unit/test_equation.cpp
    unit/test_max_heap.cpp
    unit/test_propagation.cpp
    unit/test_random.cpp
    unit/test_restart_policy.cpp
    unit/test_row_echelon_matrix.cpp
    unit/test_stats_sink.cpp
    unit/test_trail.cpp
    unit/test_trail_basis.cpp)

target_link_libraries(xorcle_tests PRIVATE xorcle_core doctest::doctest)

add_test(NAME algebra COMMAND xorcle_tests)
