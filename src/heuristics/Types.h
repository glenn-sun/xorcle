#pragma once

#include <cstddef>

namespace heuristics {

/// @brief Clause identifier type (index into clause storage).
using ClauseId = std::size_t;

/// @brief Equation identifier type (index within a clause).
using EquationId = std::size_t;

} // namespace heuristics
