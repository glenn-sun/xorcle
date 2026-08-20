#include "heuristics/compaction/original_variables.h"

#include <cassert>
#include <utility>

#include "algebra/equation.h"

namespace xorcle {

namespace {

IndexSet scatter_through_map(const IndexSet& source, const std::vector<std::size_t>& index_map,
                             std::size_t destination_num_vars) {
    assert(source.size() == index_map.size() &&
           "the variable map is not sized by the assignment's variable count");
    IndexSet destination(destination_num_vars);
    for (std::size_t index = 0; index < index_map.size(); ++index) {
        if (source.test(index)) {
            assert(index_map[index] < destination_num_vars &&
                   "the variable map points outside the original numbering");
            destination.set(index_map[index]);
        }
    }
    return destination;
}

/// @brief Undo one compaction step in place.
void undo_one_compaction(IndexSet& assignment, const CompactionHistoryEntry& entry) {
    const std::size_t old_num_vars = entry.transform.old_num_vars();
    assert(assignment.size() == entry.transform.new_num_vars() &&
           "compaction history does not line up with the assignment's variable count");

    IndexSet before(old_num_vars);
    for (std::size_t new_index = 0; new_index < entry.transform.new_num_vars(); ++new_index) {
        if (assignment.test(new_index)) {
            before.set(entry.transform.to_old(new_index));
        }
    }

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
            eliminated_value ^= (bit == old_num_vars) ? true : before.test(bit);
        }

        if (eliminated_value) {
            before.set(eliminated_var);
        } else {
            before.reset(eliminated_var);
        }
    }

    assignment = std::move(before);
}

} // namespace

IndexSet reconstruct_original_assignment(
    const IndexSet& active_assignment,
    const std::vector<CompactionHistoryEntry>& compaction_history,
    const std::vector<std::size_t>& active_var_to_original, std::size_t original_num_vars) {
    assert((compaction_history.empty()
                ? active_assignment.size() == active_var_to_original.size()
                : active_assignment.size() ==
                      compaction_history.back().transform.new_num_vars()) &&
           "the assignment is not indexed by the current active variable numbering");

    IndexSet assignment = active_assignment;

    for (auto entry = compaction_history.rbegin(); entry != compaction_history.rend(); ++entry) {
        undo_one_compaction(assignment, *entry);
    }

    if (assignment.size() == original_num_vars) {
        return assignment;
    }
    // No compaction ever ran, so the active variable numbering is the original
    // one up to the variable map the solver has been carrying.
    assert(assignment.size() == active_var_to_original.size() &&
           "failed to recover the original assignment's variable count");
    return scatter_through_map(assignment, active_var_to_original, original_num_vars);
}

} // namespace xorcle
