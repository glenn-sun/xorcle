#pragma once

/// @file original_variables.h
/// @brief The inverse of compaction, for reporting a model over the input's variables.

#include <cstddef>
#include <span>
#include <vector>

#include "heuristics/compaction/compaction.h"
#include "util/index_set.h"

namespace xorcle {

/// @brief Rewrite an assignment over the active variables into the original numbering,
/// its unused variables false.
/// @pre `compaction_history` runs oldest first, from the numbering in which variable `i`
/// is `used_vars[i]`, and each entry's rows are over the numbering the latest
/// renumbering before it left.
/// @pre `active_assignment` is indexed by active variable.
/// @pre Every entry of `used_vars` is `< original_num_vars`.
IndexSet reconstruct_original_assignment(
    const IndexSet& active_assignment,
    const std::vector<CompactionHistoryEntry>& compaction_history,
    std::span<const std::size_t> used_vars, std::size_t original_num_vars);

} // namespace xorcle
