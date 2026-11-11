#pragma once

/// @file pivot_table.h
/// @brief Gaussian elimination over a concrete `GF2Vector`, for rows each led by its lowest bit.

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "algebra/gf2_vector.h"
#include "util/index_set.h"

namespace xorcle {

/// @brief The pivot columns of rows in row-echelon form, each the lowest bit of its row, with that row's index.
///
/// The rows themselves are the caller's, so that one table serves rows kept anywhere.
class PivotTable {
public:
    explicit PivotTable(std::size_t cols) : row_idx_of_col_idx_(cols, no_row_idx), pivot_col_idxs_(cols) {}

    /// @pre @p col_idx is not a pivot column.
    void set(std::size_t col_idx, std::size_t row_idx) {
        assert(!pivot_col_idxs_.test(col_idx) && "the column is a pivot column already");
        row_idx_of_col_idx_[col_idx] = static_cast<std::uint32_t>(row_idx);
        pivot_col_idxs_.set(col_idx);
    }

    void reset(std::size_t col_idx) {
        row_idx_of_col_idx_[col_idx] = no_row_idx;
        pivot_col_idxs_.reset(col_idx);
    }

    /// @brief Reduce @p row by the rows whose pivot columns it has from column @p from_col_idx on, lowest
    /// first, calling @p add_row with each such row's index to add that row to @p row.
    template <GF2Vector V, class AddRow>
    void reduce(V& row, std::size_t from_col_idx, AddRow&& add_row) const {
        for (std::optional<std::size_t> col_idx = row.first_bit_in(pivot_col_idxs_, from_col_idx);
             col_idx.has_value(); col_idx = row.first_bit_in(pivot_col_idxs_, *col_idx + 1)) {
            add_row(static_cast<std::size_t>(row_idx_of_col_idx_[*col_idx]));
        }
    }

private:
    static constexpr std::uint32_t no_row_idx = static_cast<std::uint32_t>(-1);

    std::vector<std::uint32_t> row_idx_of_col_idx_;
    IndexSet pivot_col_idxs_;
};

} // namespace xorcle
