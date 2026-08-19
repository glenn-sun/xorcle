#pragma once

#include <cstddef>
#include <limits>
#include <vector>

#include "util/column_set.h"

namespace xorcle {

/// @brief Describes one variable-numbering compaction step.
///
/// Compaction eliminates the variables that the trail has already pivoted on,
/// renumbering the survivors into a smaller active numbering. This type is the
/// resulting change of basis; applying it to an equation is `Equation::compact()`
/// (old numbering to compacted) and `Equation::uncompact()` (compacted back to old).
///
/// Invariants: `pivot_columns` has width `old_num_vars + 1`, the constant column
/// is never a pivot, and `old_to_new` / `new_to_old` are inverse maps on the
/// surviving non-pivot variables.
class CompactionTransform {
public:
    /// @brief Sentinel for "this old variable was eliminated".
    static constexpr std::size_t npos = std::numeric_limits<std::size_t>::max();

    CompactionTransform() = default;

    /// @brief Build the transform eliminating the given pivots of the old numbering.
    /// @pre `pivots.size() == old_num_vars + 1` and does not mark the constant
    /// column.
    static CompactionTransform from_pivots(std::size_t old_num_vars, const ColumnSet& pivots);

    std::size_t old_num_vars() const noexcept { return old_num_vars_; }

    std::size_t new_num_vars() const noexcept { return new_num_vars_; }

    /// @brief The eliminated (pivot) columns, in the old variable numbering.
    const ColumnSet& pivot_columns() const noexcept { return pivot_columns_; }

    /// @brief Compacted index of a variable of the old numbering, or `npos` if eliminated.
    /// @pre `old_index < old_num_vars()`.
    std::size_t to_new(std::size_t old_index) const noexcept { return old_to_new_[old_index]; }

    /// @pre `new_index < new_num_vars()`.
    std::size_t to_old(std::size_t new_index) const noexcept { return new_to_old_[new_index]; }

private:
    std::size_t old_num_vars_ = 0;
    std::size_t new_num_vars_ = 0;
    ColumnSet pivot_columns_;
    std::vector<std::size_t> old_to_new_;
    std::vector<std::size_t> new_to_old_;
};

} // namespace xorcle
