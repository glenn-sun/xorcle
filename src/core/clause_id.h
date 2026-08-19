#pragma once

#include <cstddef>

namespace xorcle {

/// @brief Identifier of a clause: its index in the clause store.
///
/// Core rather than heuristic vocabulary because the trail records one per
/// propagated equation, even though the clause store itself lives in
/// `heuristics/selection/`.
using ClauseId = std::size_t;

/// @brief A row index into a clause, used for watch bookkeeping. Clause-relative
/// because a row index into the trail span, which `order_map()` deals in, is a
/// different quantity.
using ClauseRowIndex = std::size_t;

} // namespace xorcle
