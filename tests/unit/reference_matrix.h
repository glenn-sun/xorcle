#pragma once

#include <cassert>
#include <cstddef>
#include <optional>
#include <vector>

#include "algebra/equation.h"
#include "algebra/equation_repr.h"
#include "algebra/reduce_result.h"
#include "algebra_test_support.h"

namespace xorcle {

/// @brief A row as a plain vector of `num_vars + 1` bits.
using Bits = std::vector<bool>;

std::vector<std::size_t> set_bits(const Bits& bits);

Equation to_equation(EquationRepr repr, const Bits& bits);

Bits to_bits(const Equation& equation);

/// @brief A row with each bit set independently with probability `1 / density`.
Bits random_bits(TestRandom& random, std::size_t num_vars, std::size_t density);

/// @brief Translate a partial-reduction sentinel into a `ReduceResult`.
///
/// `leading` is `std::nullopt` for `0 = 0`, otherwise the leading column, where
/// `num_vars` -- the constant column index -- means `0 = 1`.
ReduceResult expected_result(const std::optional<std::size_t>& leading, std::size_t num_vars);

/// @brief A deliberately naive row-echelon matrix, used as an oracle.
class ReferenceMatrix {
public:
    explicit ReferenceMatrix(std::size_t num_vars)
        : num_vars_(num_vars), pivot_(num_vars + 1, false), col_idx_to_row_idx_(num_vars + 1, 0) {}

    std::size_t rows() const { return rows_.size(); }

    /// @brief Row `index`, in row-echelon order.
    /// @pre `index < rows()`.
    const Bits& row(std::size_t index) const {
        assert(index < rows() && "row index is out of range");
        return rows_[index];
    }

    /// @pre `col_idx < num_vars + 1`.
    bool is_pivot(std::size_t col_idx) const {
        assert(col_idx < pivot_.size() && "pivot column index is out of range");
        return pivot_[col_idx];
    }

    std::optional<std::size_t> add_row_get_row_idx(const Bits& v);

    void reduce(Bits& v) const;

    /// @brief Reduce, reporting through the sentinel `expected_result()` takes.
    std::optional<std::size_t> partial_reduce(Bits& v) const;

    /// @pre `rows() > 0`.
    Bits pop_first_row();

    /// @pre `rows() > 0`.
    void pop_last_row();

    /// @pre `row_idx < rows()`.
    Bits pop_row(std::size_t row_idx);

    /// @pre `row_idx < rows()`, and `v` leads strictly after the row.
    void add_to_row(std::size_t row_idx, const Bits& v);

    /// @brief Bring the rows to reduced row-echelon form.
    void reduce_rows();

    /// @brief The columns some row carries, ascending.
    std::vector<std::size_t> carried_col_idxs() const;

private:
    /// @brief Rebuild the pivot tables from the rows' leading bits.
    void index_pivots();

    std::size_t num_vars_;
    std::vector<Bits> rows_;
    std::vector<bool> pivot_;
    std::vector<std::size_t> col_idx_to_row_idx_;
};

} // namespace xorcle
