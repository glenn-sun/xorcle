cmake_minimum_required(VERSION 3.31)
project(xorcle VERSION 0.2.0 LANGUAGES CXX)

# The version `--version` and the `--help` header report.
set(XORCLE_VERSION "${PROJECT_VERSION}")

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

include(FetchContent)

add_library(xorcle_core
    src/algebra/compaction_transform.cpp
    src/algebra/equation.cpp
    src/algebra/equation_repr.cpp
    src/algebra/row_echelon_matrix.cpp
    src/algebra/dense/dense_gf2_vector.cpp
    src/algebra/sparse/sparse_gf2_vector.cpp
    src/core/analysis.cpp
    src/core/clause_database.cpp
    src/core/implication_graph_proof.cpp
    src/core/propagation.cpp
    src/core/root_visit.cpp
    src/core/saved_trail.cpp
    src/core/solver.cpp
    src/core/inproc_subsume_strengthen.cpp
    src/core/trail.cpp
    src/core/trail_basis.cpp
    src/core/vivify.cpp
    src/heuristics/bve/bve.cpp
    src/heuristics/bve/gate_solver.cpp
    src/heuristics/compaction/compaction.cpp
    src/heuristics/compaction/original_variables.cpp
    src/heuristics/equivalent_clauses/equivalent_clause_table.cpp
    src/heuristics/decisions/clause_decisions.cpp
    src/heuristics/decisions/csids.cpp
    src/heuristics/decisions/saved_trails.cpp
    src/heuristics/decisions/phase.cpp
    src/heuristics/deletion/deletion.cpp
    src/heuristics/implication_graph/implication_graph.cpp
    src/heuristics/implication_graph/span_intersection.cpp
    src/heuristics/restarts/lbd_restarts.cpp
    src/heuristics/restarts/luby.cpp
    src/heuristics/restarts/restart_policy.cpp
    src/heuristics/restarts/stabilize.cpp
    src/heuristics/selection/clause_selector.cpp
    src/heuristics/selection/cyclic.cpp
    src/heuristics/selection/exact_watchlist.cpp
    src/io/dimacs.cpp
    src/io/proof_writer.cpp
    src/util/index_set.cpp
    src/util/progress_reporter.cpp
    src/util/random.cpp
    src/util/stats.cpp
    src/util/stats_writer.cpp)

target_include_directories(xorcle_core PUBLIC src)

# Frame pointers, which a sampling profiler needs to walk the stack and the
# optimizer otherwise drops. RelWithDebInfo is the profiling configuration.
if(MSVC)
    target_compile_options(xorcle_core PUBLIC $<$<CONFIG:RelWithDebInfo>:/Oy->)
else()
    target_compile_options(xorcle_core PUBLIC
        $<$<CONFIG:RelWithDebInfo>:-fno-omit-frame-pointer;-fno-optimize-sibling-calls>)
endif()

# The x86-64 baseline is SSE2 with no hardware `popcount`; arm64's already carries
# both, which is why only this target has to ask. Added only if the compiler takes it.
option(XORCLE_NATIVE "Build for the host CPU on x86-64" ON)
if(XORCLE_NATIVE AND NOT CMAKE_CROSSCOMPILING AND NOT MSVC
        AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    include(CheckCXXCompilerFlag)
    check_cxx_compiler_flag(-march=native XORCLE_HAS_MARCH_NATIVE)
    if(XORCLE_HAS_MARCH_NATIVE)
        target_compile_options(xorcle_core PUBLIC -march=native)
    endif()
endif()

add_executable(xorcle src/cli/main.cpp)
target_link_libraries(xorcle PRIVATE xorcle_core)
target_compile_definitions(xorcle PRIVATE XORCLE_VERSION="${XORCLE_VERSION}")

# The LRUP(⊕) proof checker, one Rust file built beside the solver by rustc 1.73 or later.
# Without it the checker is not built and the tests that check proofs are not registered.
find_program(XORCLE_RUSTC rustc)
set(rustc_usable FALSE)
if(XORCLE_RUSTC)
    execute_process(COMMAND ${XORCLE_RUSTC} --version OUTPUT_VARIABLE rustc_version)
    string(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+" rustc_version "${rustc_version}")
    if(rustc_version VERSION_LESS 1.73)
        message(STATUS "rustc ${rustc_version} is older than 1.73: the proof checker is not built")
    else()
        set(rustc_usable TRUE)
    endif()
else()
    message(STATUS "No rustc: the proof checker is not built")
endif()
if(rustc_usable)
    set(XORCLE_PROOF_CHECKER ${CMAKE_BINARY_DIR}/check_lrup_parity${CMAKE_EXECUTABLE_SUFFIX})
    add_custom_command(OUTPUT ${XORCLE_PROOF_CHECKER}
        COMMAND ${XORCLE_RUSTC} --edition 2021 -O -o ${XORCLE_PROOF_CHECKER}
                ${CMAKE_SOURCE_DIR}/tools/check_lrup_parity.rs
        DEPENDS ${CMAKE_SOURCE_DIR}/tools/check_lrup_parity.rs
        VERBATIM)
    add_custom_target(check_lrup_parity ALL DEPENDS ${XORCLE_PROOF_CHECKER})
endif()

enable_testing()
add_subdirectory(tests)

# Warnings for xorcle's own targets only; doctest is SYSTEM.
option(XORCLE_WERROR "Treat warnings in xorcle's own targets as errors" OFF)
if(NOT MSVC)
    set(XORCLE_WARNINGS
        -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Wold-style-cast
        -Wnon-virtual-dtor -Woverloaded-virtual -Wsuggest-override -Wextra-semi -Wcast-qual
        -Wformat=2 -Wimplicit-fallthrough -Wundef -Wredundant-decls -Wmissing-declarations
        -Wnull-dereference
        $<$<CXX_COMPILER_ID:GNU>:-Wduplicated-cond;-Wduplicated-branches;-Wlogical-op>
        $<$<BOOL:${XORCLE_WERROR}>:-Werror>)
    foreach(target xorcle_core xorcle xorcle_tests)
        target_compile_options(${target} PRIVATE ${XORCLE_WARNINGS})
    endforeach()
endif()
