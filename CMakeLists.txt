cmake_minimum_required(VERSION 3.31)
project(xorcle VERSION 0.2.0 LANGUAGES CXX)

# Nothing on this branch is released. `PROJECT_VERSION` is where it is heading,
# and the `-dev` suffix is what says it has not arrived; a release drops it.
set(XORCLE_VERSION "${PROJECT_VERSION}-dev")

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

include(FetchContent)

# gf2 (nessan/gf2) -- the dense representation's bit-vector library.
#
# Pinned to a commit rather than a tag: the only tagged release, v1.0.1, predates
# the rename of `BitVec` to `BitVector`, and the `current` tag is a rolling
# release asset.
FetchContent_Declare(gf2
    GIT_REPOSITORY https://github.com/nessan/gf2.git
    GIT_TAG        0b09e44fd2c6615a6a1958c480650e207703b84c
    SYSTEM)
FetchContent_MakeAvailable(gf2)

add_library(xorcle_core
    src/algebra/compaction_transform.cpp
    src/algebra/equation.cpp
    src/algebra/equation_repr.cpp
    src/algebra/row_echelon_matrix.cpp
    src/algebra/dense/dense_gf2_vector.cpp
    src/algebra/dense/dense_repr.cpp
    src/algebra/sparse/sparse_gf2_vector.cpp
    src/algebra/sparse/sparse_repr.cpp
    src/core/analysis.cpp
    src/core/propagation.cpp
    src/core/solver.cpp
    src/core/trail.cpp
    src/core/trail_basis.cpp
    src/heuristics/compaction/compaction.cpp
    src/heuristics/compaction/original_variables.cpp
    src/heuristics/decisions/clause_decisions.cpp
    src/heuristics/decisions/csids.cpp
    src/heuristics/decisions/decision_procedure.cpp
    src/heuristics/decisions/fixed_literals.cpp
    src/heuristics/decisions/phase.cpp
    src/heuristics/decisions/variable_vsids.cpp
    src/heuristics/restarts/geometric.cpp
    src/heuristics/restarts/luby.cpp
    src/heuristics/restarts/restart_policy.cpp
    src/heuristics/selection/clause_selector.cpp
    src/heuristics/selection/cyclic.cpp
    src/heuristics/selection/leading_term_watchlist.cpp
    src/io/dimacs.cpp
    src/io/lrup_logger.cpp
    src/util/index_set.cpp
    src/util/progress_reporter.cpp
    src/util/random.cpp
    src/util/stats.cpp
    src/util/stats_writer.cpp)

target_link_libraries(xorcle_core PUBLIC gf2::gf2)
target_include_directories(xorcle_core PUBLIC src)

# Frame pointers, which a sampling profiler needs to walk the stack and the
# optimizer otherwise drops. RelWithDebInfo is the profiling configuration.
if(MSVC)
    target_compile_options(xorcle_core PUBLIC $<$<CONFIG:RelWithDebInfo>:/Oy->)
else()
    target_compile_options(xorcle_core PUBLIC
        $<$<CONFIG:RelWithDebInfo>:-fno-omit-frame-pointer;-fno-optimize-sibling-calls>)
endif()

# gf2 gates its bounds checks on `gf2_debug_assert`, which is a no-op unless
# DEBUG is defined -- and neither CMake's Debug build type nor gf2's own
# FetchContent-consumed CMakeLists defines it.
target_compile_definitions(xorcle_core PRIVATE $<$<CONFIG:Debug>:DEBUG>)

add_executable(xorcle src/cli/main.cpp)
target_link_libraries(xorcle PRIVATE xorcle_core)
target_compile_definitions(xorcle PRIVATE XORCLE_VERSION="${XORCLE_VERSION}")

enable_testing()
add_subdirectory(tests)
