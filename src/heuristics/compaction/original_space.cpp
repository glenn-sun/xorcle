#include "heuristics/compaction/original_space.h"

#include <cassert>
#include <utility>

#include "algebra/equation.h"

namespace xorcle {

namespace {

ColumnSet scatter_through_map(const ColumnSet& source, const std::vector<std::size_t>& index_map,
                           std::size_t destination_width) {
    assert(source.size() == index_map.size());
    ColumnSet destination(destination_width);
    for (std::size_t index = 0; index < index_map.size(); ++index) {
        if (source.test(index)) {
            assert(index_map[index] < destination_width);
            destination.set(index_map[index]);
        }
    }
    return destination;
}

void undo_one_compaction(ColumnSet& assignment, const CompactionHistoryEntry& entry,
                         std::vector<std::size_t>& bit_scratch) {
    const std::size_t old_num_vars = entry.transform.old_num_vars();
    assert(assignment.size() == entry.transform.new_num_vars() &&
           "compaction history does not line up with the assignment width");

    ColumnSet before(old_num_vars);
    for (std::size_t new_index = 0; new_index < entry.transform.new_num_vars(); ++new_index) {
        if (assignment.test(new_index)) {
            before.set(entry.transform.to_old(new_index));
        }
    }

    for (auto row = entry.trail_rows_before.rbegin(); row != entry.trail_rows_before.rend();
         ++row) {
        // One coarse call per row: the set bits come back ascending, so the
        // first is the pivot variable and the last is the constant bit when the
        // row has one.
        row->bits(bit_scratch);
        assert(!bit_scratch.empty() && "compaction history holds a zero trail row");
        const std::size_t pivot_var = bit_scratch.front();
        assert(pivot_var < old_num_vars && "compaction history holds an invalid trail pivot");

        bool pivot_value = false;
        for (std::size_t i = 1; i < bit_scratch.size(); ++i) {
            const std::size_t bit = bit_scratch[i];
            pivot_value ^= (bit == old_num_vars) ? true : before.test(bit);
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

ColumnSet reconstruct_original_assignment(
    const ColumnSet& active_assignment,
    const std::vector<CompactionHistoryEntry>& compaction_history,
    const std::vector<std::size_t>& active_var_to_original, std::size_t original_num_vars) {
    ColumnSet assignment = active_assignment;
    std::vector<std::size_t> bit_scratch;

    for (auto entry = compaction_history.rbegin(); entry != compaction_history.rend(); ++entry) {
        undo_one_compaction(assignment, *entry, bit_scratch);
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
