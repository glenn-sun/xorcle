#include "heuristics/compaction/original_variables.h"

#include <cassert>
#include <cstdint>
#include <span>
#include <utility>

namespace xorcle {

namespace {

/// @brief Whether @p assignment satisfies the clause with linear negation @p negation.
bool satisfies(const IndexSet& assignment, const std::vector<VarListEquation>& negation) {
    for (const VarListEquation& row : negation) {
        bool sum = false;
        for (const std::uint32_t var : row.vars) {
            sum = sum != assignment.test(var);
        }
        if (sum != row.constant) {
            return true;
        }
    }
    return false;
}

bool satisfies_all(const IndexSet& assignment,
                   const std::vector<std::vector<VarListEquation>>& negations) {
    for (const std::vector<VarListEquation>& negation : negations) {
        if (!satisfies(assignment, negation)) {
            return false;
        }
    }
    return true;
}

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

    // An assignment that satisfies the resolvents satisfies the clauses an
    // elimination removed either as it is or with the flip set flipped.
    for (auto elimination = entry.eliminations.rbegin(); elimination != entry.eliminations.rend();
         ++elimination) {
        if (satisfies_all(assignment, elimination->removed_clauses)) {
            continue;
        }
        for (const std::uint32_t var : elimination->flip_set) {
            if (assignment.test(var)) {
                assignment.reset(var);
            } else {
                assignment.set(var);
            }
        }
        assert(satisfies_all(assignment, elimination->removed_clauses) &&
               "neither side of a flip set satisfies the clauses its elimination removed");
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
