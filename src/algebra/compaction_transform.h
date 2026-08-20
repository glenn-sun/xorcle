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

    /// @brief The dropped columns in the old variable numbering.
    const IndexSet& dropped_columns() const noexcept { return dropped_columns_; }

    /// @brief New numbering of a variable from the old numbering, or `npos` if dropped.
    /// @pre `old_index < old_num_vars()`. The constant column has no image here;
    /// a caller mapping a whole equation handles bit `old_num_vars()` itself.
    std::size_t to_new(std::size_t old_index) const noexcept {
        assert(old_index < old_num_vars_ && "to_new: index is not in the old numbering");
        return old_to_new_[old_index];
    }

    /// @pre `new_index < new_num_vars()`, i.e. the constant column is excluded here too.
    std::size_t to_old(std::size_t new_index) const noexcept {
        assert(new_index < new_num_vars_ && "to_old: index is not in the new numbering");
        return new_to_old_[new_index];
    }

private:
    std::size_t old_num_vars_ = 0;
    std::size_t new_num_vars_ = 0;
    IndexSet dropped_columns_;
    std::vector<std::size_t> old_to_new_;
    std::vector<std::size_t> new_to_old_;
};

} // namespace xorcle
