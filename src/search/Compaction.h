#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <vector>

#include "algebra/Equation.h"
#include "algebra/RowSpace.h"
#include "bit/vector.h"
#include "heuristics/Types.h"
#include "search/Trail.h"

namespace search {

/// @brief Describes one variable-space compaction step.
/// @note `pivot_cols` has width `old_num_vars + 1`, the constant column is never a
/// pivot, and `old_to_new` / `new_to_old` are inverse maps on the surviving
/// non-pivot variables.
struct CompactionTransform {
    static constexpr std::size_t npos = std::numeric_limits<std::size_t>::max();

    std::size_t old_num_vars = 0;
    std::size_t new_num_vars = 0;

    /// @brief Pivot columns in old space (size old_num_vars + 1).
    bit::vector<> pivot_cols;

    /// @brief Mapping old variable index -> new variable index, or npos if eliminated.
    std::vector<std::size_t> old_to_new;

    /// @brief Mapping new variable index -> old variable index.
    std::vector<std::size_t> new_to_old;

    /// @brief Build a compaction transform from old-space pivot columns.
    /// @param old_vars Number of variables in the old active space.
    /// @param pivots Pivot columns in old-space row-echelon order.
    /// @return Transform eliminating the pivot variables.
    /// @throws std::invalid_argument If `pivots` has the wrong width or marks the constant column.
    static CompactionTransform from_pivots(std::size_t old_vars, const bit::vector<>& pivots) {
        if (pivots.size() != old_vars + 1) {
            throw std::invalid_argument("CompactionTransform::from_pivots: pivot width mismatch");
        }
        if (pivots.element(old_vars)) {
            throw std::invalid_argument("CompactionTransform::from_pivots: constant column cannot be pivot");
        }

        CompactionTransform out;
        out.old_num_vars = old_vars;
        out.pivot_cols = pivots;
        out.old_to_new.assign(old_vars, npos);
        out.new_to_old.clear();
        out.new_to_old.reserve(old_vars);

        for (std::size_t i = 0; i < old_vars; ++i) {
            if (pivots.element(i)) {
                continue;
            }
            std::size_t mapped = out.new_to_old.size();
            out.old_to_new[i] = mapped;
            out.new_to_old.push_back(i);
        }
        out.new_num_vars = out.new_to_old.size();
#ifndef NDEBUG
        for (std::size_t old_idx = 0; old_idx < old_vars; ++old_idx) {
            const std::size_t mapped = out.old_to_new[old_idx];
            if (mapped == npos) {
                continue;
            }
            if (mapped >= out.new_num_vars || out.new_to_old[mapped] != old_idx) {
                throw std::logic_error("CompactionTransform::from_pivots: mapping is not invertible");
            }
        }
#endif
        return out;
    }

    /// @brief Return whether an old-space variable is eliminated by compaction.
    /// @param old_idx Old-space variable index.
    /// @return True if the variable is a pivot variable.
    [[nodiscard]] bool is_pivot_old(std::size_t old_idx) const {
        return old_idx < old_num_vars && pivot_cols.element(old_idx);
    }

    /// @brief Project an equation from the old active space into the compacted space.
    /// @param old_eqn Equation in the old active space.
    /// @return Projected equation in the compacted active space.
    /// @pre `old_eqn.size() == old_num_vars + 1`.
    [[nodiscard]] algebra::Equation project_equation(const algebra::Equation& old_eqn) const {
        if (old_eqn.size() != old_num_vars + 1) {
            throw std::invalid_argument("CompactionTransform::project_equation: old equation width mismatch");
        }

        algebra::Equation out{new_num_vars};
        for (std::size_t old_idx = 0; old_idx < old_num_vars; ++old_idx) {
            if (!old_eqn.element(old_idx)) {
                continue;
            }
            std::size_t mapped = old_to_new[old_idx];
            if (mapped != npos) {
                out.set(mapped);
            }
        }
        if (old_eqn.element(old_num_vars)) {
            out.set(new_num_vars);
        }
        return out;
    }

    /// @brief Lift an equation from the compacted space back to the pre-compaction space.
    /// @param new_eqn Equation in the compacted active space.
    /// @return Lifted equation in the old active space.
    /// @pre `new_eqn.size() == new_num_vars + 1`.
    [[nodiscard]] algebra::Equation lift_equation(const algebra::Equation& new_eqn) const {
        if (new_eqn.size() != new_num_vars + 1) {
            throw std::invalid_argument("CompactionTransform::lift_equation: new equation width mismatch");
        }

        algebra::Equation out{old_num_vars};
        for (std::size_t new_idx = 0; new_idx < new_num_vars; ++new_idx) {
            if (new_eqn.element(new_idx)) {
                out.set(new_to_old[new_idx]);
            }
        }
        if (new_eqn.element(new_num_vars)) {
            out.set(old_num_vars);
        }
        return out;
    }
};

/// @brief Original-space equation that must be appended to later proof lines after compaction.
struct ProofAssumption {
    algebra::Equation original_eqn;
    std::optional<heuristics::ClauseId> reason_clause_id;
};

/// @brief One committed compaction step recorded for later lifting back to original space.
struct CompactionHistoryEntry {
    CompactionTransform transform;
    std::vector<std::size_t> active_var_to_original_before;
    std::vector<algebra::Equation> trail_rows_before;
};

/// @brief Fully prepared replacement state for one compaction.
/// @note This is built before mutating the live solver state so a reader can see every
/// output of compaction in one place: rewritten clauses, new active flags, proof assumptions,
/// and the history entry needed for later lifting.
struct CompactionPlan {
    CompactionTransform transform;
    std::vector<algebra::Clause> rewritten_clauses;
    std::vector<std::uint8_t> clause_is_active;
    std::vector<ProofAssumption> proof_assumptions;
    CompactionHistoryEntry history_entry;
};

/// @brief Read-only view of the current active solver state needed to plan compaction.
struct CompactionInputs {
    const Trail& trail;
    const std::vector<algebra::Clause>& clauses;
    const std::vector<std::uint8_t>& clause_is_active;
    const std::vector<std::size_t>& active_var_to_original;
    const std::vector<ProofAssumption>& existing_proof_assumptions;
    std::size_t active_num_vars;
    std::size_t original_num_vars;
    bool has_compacted_once;
    std::size_t num_conflicts;
    std::size_t conflicts_at_last_compaction;
    std::size_t compaction_conflict_interval;
};

/// @brief Return true when compaction should be attempted before the next decision.
bool should_compact_before_decision(const CompactionInputs& inputs);

/// @brief Build the compaction transform from the current trail pivots.
CompactionTransform build_compaction_transform(const Trail& trail, std::size_t active_num_vars);

/// @brief Rewrite the current active problem into the compacted variable space.
/// @param inputs Read-only view of the current active solver state.
/// @param transform Transform from current active variables into compacted active variables.
/// @return Full compaction plan ready for the solver to apply.
CompactionPlan build_compaction_plan(const CompactionInputs& inputs,
                                     const CompactionTransform& transform);

} // namespace search
