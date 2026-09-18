#pragma once

#include <cstddef>

namespace xorcle {

/// @brief Identifier of a clause: its index in the clause store.
using ClauseId = std::size_t;

/// @brief A row index into a clause, not into the trail span.
using ClauseRowIdx = std::size_t;

} // namespace xorcle
