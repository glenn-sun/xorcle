#pragma once

/// @file saved_trail.h
/// @brief A trail saved by level for later reuse, rewritten with the clauses at a
/// substitution and a renumbering.

#include <cstddef>
#include <span>
#include <vector>

#include "algebra/compaction_transform.h"
#include "algebra/equation.h"
#include "algebra/row_echelon_matrix.h"
#include "core/trail.h"

namespace xorcle {

/// @brief A level stamp no trail level has; the solver's stamps start at 1.
inline constexpr std::size_t unshared_level_stamp = 0;

/// @brief Rows of a trail above its root, in trail order, with the stamp of each level,
/// so that the levels it shares with the current trail are known.
struct SavedTrail {
    /// @brief The first `num_rows()` are saved; any after them are spare storage.
    std::vector<Equation> rows;
    /// @brief `level_ends[k]` is how many of `rows` lie at levels `k + 1` or below.
    std::vector<std::size_t> level_ends;
    std::vector<std::size_t> level_stamps; ///< Of levels `0` to `level_ends.size()`.
    /// @brief Rows at the root when saved, the first rows of the current root.
    std::size_t root_rows = 0;

    std::size_t num_rows() const noexcept { return level_ends.empty() ? 0 : level_ends.back(); }

    /// @brief Copy the levels `1` to `num_levels` of `trail`, up to trail position `end`.
    /// @param trail_stamps The stamp of every level of `trail`.
    void save(const Trail& trail, std::span<const std::size_t> trail_stamps,
              std::size_t num_levels, std::size_t end);

    /// @brief Copy the decision of each of the levels `1` to `num_levels` of `trail`,
    /// one row per level.
    /// @pre Each of those levels opens with its decision.
    void save_decisions(const Trail& trail, std::span<const std::size_t> trail_stamps,
                        std::size_t num_levels);

    /// @brief Rewrite the rows as a substitution rewrites the clauses: reduce each by the
    /// root rows `root_span` it substitutes, in place, so that a row the root implies
    /// becomes `0 = 0` and one it contradicts holds the constant alone.
    ///
    /// The root keeps none of the saved trail's rows afterwards, and no level is shared.
    /// @pre `root_span` holds the trail's root rows, the `root_rows` first among them.
    void substitute(const RowSpace& root_span);

    /// @brief Renumber the rows as a renumbering renumbers the clauses.
    /// @pre No row carries a dropped variable, and `root_rows` is zero.
    void renumber(const CompactionTransform& transform);
};

} // namespace xorcle
