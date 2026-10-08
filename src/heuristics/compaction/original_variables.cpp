#include "heuristics/compaction/original_variables.h"

#include <cassert>
#include <span>
#include <utility>

#include "algebra/equation.h"

namespace xorcle {

namespace {

/// @brief Undo one compaction step in place.
void undo_one_compaction(IndexSet& assignment, const CompactionHistoryEntry& entry) {
    if (entry.renumbering.has_value()) {
        const CompactionTransform& transform = *entry.renumbering;
        assert(assignment.size() == transform.new_num_vars() &&
               "compaction history does not line up with the assignment's variable count");
        IndexSet renumbered(transform.old_num_vars());
        assignment.for_each_set(
            [&](std::size_t new_col_idx) { renumbered.set(transform.to_old(new_col_idx)); });
        assignment = std::move(renumbered);
    }
    const std::size_t old_num_vars = assignment.size();

    for (auto row = entry.trail_rows_before.rbegin(); row != entry.trail_rows_before.rend();
         ++row) {
        // One coarse call per row: the set bits come back ascending, so the
        // first is the eliminated variable and the last is the constant bit when
        // the row has one.
        const std::vector<std::size_t> row_bits = row->bits();
        assert(!row_bits.empty() && "compaction history holds a zero trail row");
        const std::size_t eliminated_var = row_bits.front();
        assert(eliminated_var < old_num_vars &&
               "compaction history holds an invalid eliminated variable");

        bool eliminated_value = false;
        for (std::size_t i = 1; i < row_bits.size(); ++i) {
            const std::size_t bit = row_bits[i];
            eliminated_value ^= (bit == old_num_vars) ? true : assignment.test(bit);
        }

        if (eliminated_value) {
            assignment.set(eliminated_var);
        } else {
            assignment.reset(eliminated_var);
        }
    }
}

} // namespace

IndexSet reconstruct_original_assignment(
    const IndexSet& active_assignment,
    const std::vector<CompactionHistoryEntry>& compaction_history,
    std::span<const std::size_t> used_vars, std::size_t original_num_vars) {
    IndexSet assignment = active_assignment;
    for (auto entry = compaction_history.rbegin(); entry != compaction_history.rend(); ++entry) {
        undo_one_compaction(assignment, *entry);
    }
    assert(assignment.size() == used_vars.size() &&
           "the compaction history does not lead back to the used variables");

    IndexSet original_assignment(original_num_vars);
    assignment.for_each_set([&](std::size_t used_var) {
        assert(used_vars[used_var] < original_num_vars &&
               "a used variable is outside the original numbering");
        original_assignment.set(used_vars[used_var]);
    });
    return original_assignment;
}

} // namespace xorcle
