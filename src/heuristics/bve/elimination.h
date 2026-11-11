#pragma once

/// @file elimination.h
/// @brief What is kept of an elimination, enough to undo it on an assignment.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace xorcle {

/// @brief An equation as the variables it mentions, ascending, and its constant.
struct VarListEquation {
    std::vector<std::uint32_t> vars;
    bool constant = false;

    bool operator==(const VarListEquation&) const = default;
};

/// @brief One elimination: its flip set, ascending, and the clauses it removed,
/// each in its linear negation.
struct Elimination {
    std::vector<std::uint32_t> flip_set;
    std::vector<std::vector<VarListEquation>> removed_clauses;
};

} // namespace xorcle
