#pragma once

#include <cstddef>
#include <limits>
#include <stdexcept>
#include <vector>

#include "algebra/Equation.h"
#include "bit/vector.h"

namespace search {

/// @brief Describes one variable-space compaction step.
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
        return out;
    }

    [[nodiscard]] bool is_pivot_old(std::size_t old_idx) const {
        return old_idx < old_num_vars && pivot_cols.element(old_idx);
    }

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

} // namespace search

