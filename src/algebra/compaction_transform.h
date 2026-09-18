#pragma once

#include <cassert>
#include <cstddef>
#include <limits>
#include <vector>

#include "util/index_set.h"

namespace xorcle {

/// @brief Renumbers variables corresponding to one compaction step.
class CompactionTransform {
public:
    /// @brief Sentinel for dropped variables
    static constexpr std::size_t npos = std::numeric_limits<std::size_t>::max();

    CompactionTransform() = default;

    /// @brief Build the transform dropping the given variables of the old numbering.
    /// @pre `dropped_columns.size() == old_num_vars + 1` and does not mark the
    /// constant column.
    static CompactionTransform from_dropped_columns(std::size_t old_num_vars,
                                                    const IndexSet& dropped_columns);

    std::size_t old_num_vars() const noexcept { return old_num_vars_; }

    std::size_t new_num_vars() const noexcept { return new_num_vars_; }

    /// @brief New numbering of a variable from the old numbering, or `npos` if dropped.
    /// @pre `old_col_idx < old_num_vars()`. The constant column has no image here;
    /// a caller mapping a whole equation handles bit `old_num_vars()` itself.
    std::size_t to_new(std::size_t old_col_idx) const noexcept {
        assert(old_col_idx < old_num_vars_ && "to_new: old_col_idx is not in the old numbering");
        return old_col_idx_to_new_col_idx_[old_col_idx];
    }

    /// @pre `new_col_idx < new_num_vars()`, i.e. the constant column is excluded here too.
    std::size_t to_old(std::size_t new_col_idx) const noexcept {
        assert(new_col_idx < new_num_vars_ && "to_old: new_col_idx is not in the new numbering");
        return new_col_idx_to_old_col_idx_[new_col_idx];
    }

private:
    std::size_t old_num_vars_ = 0;
    std::size_t new_num_vars_ = 0;
    std::vector<std::size_t> old_col_idx_to_new_col_idx_;
    std::vector<std::size_t> new_col_idx_to_old_col_idx_;
};

} // namespace xorcle
