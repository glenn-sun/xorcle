#pragma once

/// @file original_variables.h
/// @brief The inverse of compaction: recovering an assignment over the input's
/// own variables, so a SAT answer can be reported in the caller's terms.

#include <cstddef>
#include <vector>

#include "heuristics/compaction/compaction.h"
#include "util/column_set.h"

namespace xorcle {

/// @brief Rewrite an assignment over the active variables back into the original
/// variable numbering, one bit per original variable. `compaction_history` runs
/// oldest first, and is undone one step at a time newest first: each step
/// scatters the surviving variables back to their pre-compaction indices, then
/// back-substitutes the eliminated ones from the trail rows that eliminated them
/// — for a row with leading column @f$p@f$, @f$x_p@f$ is the row's constant bit
/// plus the assignment of every other variable the row mentions.
/// @pre `active_assignment` has width `compaction_history.back().transform.new_num_vars()`,
/// or, when no compaction has run, `active_var_to_original.size()`.
///
/// @note The trail rows are walked **backwards**, and that is mandatory. They
/// were recorded in row-echelon order, so reversing makes each eliminated
/// variable the leftmost unknown in its row, everything to the right already
/// solved. Forwards is not a different answer, it is a wrong one.
ColumnSet reconstruct_original_assignment(
    const ColumnSet& active_assignment,
    const std::vector<CompactionHistoryEntry>& compaction_history,
    const std::vector<std::size_t>& active_var_to_original, std::size_t original_num_vars);

} // namespace xorcle
