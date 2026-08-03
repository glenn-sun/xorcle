#pragma once

/// @file original_space.h
/// @brief Undoing compaction: recovering an assignment over the input's own
/// variables from one found in the compacted space.

#include <cstddef>
#include <vector>

#include "heuristics/compaction/compaction.h"
#include "util/bitmap.h"

namespace xorcle {

/// @brief Lift a satisfying assignment back into the original variable space.
/// @param active_assignment One bit per *currently active* variable.
/// @param compaction_history Every compaction applied so far, oldest first.
/// @param active_var_to_original The current active-to-original variable map.
/// @param original_num_vars Number of variables in the input instance.
/// @return One bit per original variable.
/// @pre `active_assignment` has width `compaction_history.back().transform.new_num_vars()`,
/// or, when no compaction has run, `active_var_to_original.size()`.
///
/// Compaction is undone one step at a time, newest first. Each step scatters the
/// surviving variables back to their pre-compaction indices and then
/// back-substitutes the eliminated ones from the root trail rows that eliminated
/// them: for a row with leading column @f$p@f$, @f$x_p@f$ is the row's constant
/// bit XOR the assignment of every other variable the row mentions.
///
/// @note The trail rows are walked **backwards**, and that is mandatory rather
/// than stylistic. They were recorded in row-echelon order, so leading columns
/// increase along them; reversing makes each eliminated variable the leftmost
/// unknown in its row, with everything to the right already solved. Forwards is
/// not merely a different answer, it is a wrong one.
///
/// @note The 0.1 tree's `OriginalSpace` also built proof clauses and the
/// root-UNSAT proof plan in original space. Both existed only to emit proofs
/// with compaction on, which is not ported, so
/// this is all that remains: the inverse map, for reporting SAT assignments.
Bitmap reconstruct_original_assignment(
    const Bitmap& active_assignment,
    const std::vector<CompactionHistoryEntry>& compaction_history,
    const std::vector<std::size_t>& active_var_to_original, std::size_t original_num_vars);

} // namespace xorcle
