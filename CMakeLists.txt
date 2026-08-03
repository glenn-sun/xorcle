cmake_minimum_required(VERSION 3.31)
project(xorcle VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

include(FetchContent)

# gf2 (nessan/gf2) -- the dense algebra backend's bit-vector library.
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
    src/algebra/algebra.cpp
    src/algebra/backend.cpp
    src/algebra/compaction_transform.cpp
    src/algebra/equation.cpp
    src/algebra/row_echelon_matrix.cpp
    src/algebra/dense/dense_backend.cpp
    src/algebra/dense/dense_row.cpp
    src/algebra/sparse/sparse_backend.cpp
    src/algebra/sparse/sparse_row.cpp
    src/core/propagation.cpp
    src/core/trail.cpp
    src/core/trail_basis.cpp
    src/heuristics/compaction/compaction.cpp
    src/heuristics/compaction/original_space.cpp
    src/heuristics/decisions/csids.cpp
    src/heuristics/decisions/decision_procedure.cpp
    src/heuristics/decisions/decision_utils.cpp
    src/heuristics/decisions/fixed_literals.cpp
    src/heuristics/decisions/phase.cpp
    src/heuristics/decisions/variable_vsids.cpp
    src/heuristics/restarts/geometric.cpp
    src/heuristics/restarts/luby.cpp
    src/heuristics/restarts/restart_policy.cpp
    src/heuristics/selection/clause_selector.cpp
    src/heuristics/selection/cyclic.cpp
    src/heuristics/selection/leading_term_watchlist.cpp
    src/util/bitmap.cpp
    src/util/random.cpp
    src/util/stats.cpp
    src/util/stats_sink.cpp)

target_link_libraries(xorcle_core PUBLIC gf2::gf2)
target_include_directories(xorcle_core PUBLIC src)

# gf2 gates its bounds checks on `gf2_debug_assert`, which is a no-op unless
# DEBUG is defined -- and neither CMake's Debug build type nor gf2's own
# FetchContent-consumed CMakeLists defines it. Without this line a Debug build
# gets no bounds checking from gf2 at all.
target_compile_definitions(xorcle_core PRIVATE $<$<CONFIG:Debug>:DEBUG>)

add_executable(xorcle src/cli/main.cpp)
target_link_libraries(xorcle PRIVATE xorcle_core)
target_compile_definitions(xorcle PRIVATE XORCLE_VERSION="${PROJECT_VERSION}")

enable_testing()
add_subdirectory(tests)
