#pragma once

#include <cstddef>

namespace xorcle {

/// @brief Identifier of a clause: its index in the clause store.
///
/// Clause identifiers are core rather than heuristic vocabulary because the
/// trail records one per propagated equation -- `Trail::reasons()` is a vector
/// of `std::optional<ClauseId>`. The clause store itself lives in
/// `heuristics/selection/`, but the identifier it hands out is part of the
/// paper's trail.
using ClauseId = std::size_t;

/// @brief Identifier of an equation within a clause: its row index.
///
/// Used for watch bookkeeping, where a watch names one row of one clause.
using EquationId = std::size_t;

} // namespace xorcle
