#pragma once

/// @file original_variables.h
/// @brief The inverse of compaction, for reporting a model over the input's variables.

#include <cstddef>
#include <vector>

#include "heuristics/compaction/compaction.h"
#include "util/index_set.h"

namespace xorcle {

/// @brief Rewrite an assignment over the active variables into the original numbering.
/// @pre `compaction_history` runs oldest first and chains: entry `i`'s
/// `transform.new_num_vars()` is entry `i + 1`'s `old_num_vars()`.
/// @pre `active_assignment` is indexed by active variable, over
/// `compaction_history.back().transform.new_num_vars()` of them, or, with no history,
/// `active_var_to_original.size()`.
/// @pre Every entry of `active_var_to_original` is `< original_num_vars`, and every row
/// of every entry's `trail_rows_before` is non-zero with a variable leading bit.
IndexSet reconstruct_original_assignment(
    const IndexSet& active_assignment,
    const std::vector<CompactionHistoryEntry>& compaction_history,
    const std::vector<std::size_t>& active_var_to_original, std::size_t original_num_vars);

} // namespace xorcle
