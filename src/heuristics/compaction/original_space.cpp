#include "heuristics/compaction/original_space.h"

#include <cassert>
#include <utility>

#include "algebra/equation.h"

namespace xorcle {

namespace {

/// @brief Scatter an assignment through an index map into a wider space.
/// @param source One bit per index of `index_map`.
/// @param index_map Where each source index lands in the destination.
/// @param destination_width Width of the result.
Bitmap scatter_through_map(const Bitmap& source, const std::vector<std::size_t>& index_map,
                           std::size_t destination_width) {
    assert(source.size() == index_map.size());
    Bitmap destination(destination_width);
    for (std::size_t index = 0; index < index_map.size(); ++index) {
        if (source.test(index)) {
            assert(index_map[index] < destination_width);
            destination.set(index_map[index]);
        }
    }
    return destination;
}

/// @brief Undo one compaction step, in place on `assignment`.
void undo_one_compaction(Bitmap& assignment, const CompactionHistoryEntry& entry,
                         std::vector<std::size_t>& column_scratch) {
    const std::size_t old_num_vars = entry.transform.old_num_vars();
    assert(assignment.size() == entry.transform.new_num_vars() &&
           "compaction history does not line up with the assignment width");

    Bitmap before(old_num_vars);
    for (std::size_t new_index = 0; new_index < entry.transform.new_num_vars(); ++new_index) {
        if (assignment.test(new_index)) {
            before.set(entry.transform.to_old(new_index));
        }
    }

    for (auto row = entry.trail_rows_before.rbegin(); row != entry.trail_rows_before.rend();
         ++row) {
        // One coarse call per row: the set columns come back ascending, so the
        // first is the pivot variable and the last is the constant column when
        // the row has one.
        row->columns(column_scratch);
        assert(!column_scratch.empty() && "compaction history holds a zero trail row");
        const std::size_t pivot_var = column_scratch.front();
        assert(pivot_var < old_num_vars && "compaction history holds an invalid trail pivot");

        bool pivot_value = false;
        for (std::size_t i = 1; i < column_scratch.size(); ++i) {
            const std::size_t column = column_scratch[i];
            pivot_value ^= (column == old_num_vars) ? true : before.test(column);
        }

        if (pivot_value) {
            before.set(pivot_var);
        } else {
            before.reset(pivot_var);
        }
    }

    assignment = std::move(before);
}

} // namespace

Bitmap reconstruct_original_assignment(
    const Bitmap& active_assignment,
    const std::vector<CompactionHistoryEntry>& compaction_history,
    const std::vector<std::size_t>& active_var_to_original, std::size_t original_num_vars) {
    Bitmap assignment = active_assignment;
    std::vector<std::size_t> column_scratch;

    for (auto entry = compaction_history.rbegin(); entry != compaction_history.rend(); ++entry) {
        undo_one_compaction(assignment, *entry, column_scratch);
    }

    if (assignment.size() == original_num_vars) {
        return assignment;
    }
    // No compaction ever ran, so the active space is the original space up to
    // the variable map the solver has been carrying.
    assert(assignment.size() == active_var_to_original.size() &&
           "failed to recover the original assignment width");
    return scatter_through_map(assignment, active_var_to_original, original_num_vars);
}

} // namespace xorcle
