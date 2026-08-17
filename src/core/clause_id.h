#pragma once

#include <cstddef>

namespace xorcle {

/// @brief Identifier of a clause: its index in the clause store.
///
/// Core rather than heuristic vocabulary because the trail records one per
/// propagated equation, even though the clause store itself lives in
/// `heuristics/selection/`.
using ClauseId = std::size_t;

/// @brief Identifier of an equation within a clause: its row index, used for
/// watch bookkeeping.
using EquationId = std::size_t;

} // namespace xorcle
