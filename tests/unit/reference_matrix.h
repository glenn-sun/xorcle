#pragma once

#include <cstddef>
#include <optional>
#include <vector>

#include "algebra/algebra.h"
#include "algebra/equation.h"
#include "algebra/reduce_result.h"
#include "algebra_test_support.h"

namespace xorcle {

/// @brief A row as a plain vector of bits, of width `num_vars + 1`.
using Bits = std::vector<bool>;

std::optional<std::size_t> first_set(const Bits& bits);

/// @brief First set column of `bits` strictly after `after`, or `std::nullopt`.
std::optional<std::size_t> next_set(const Bits& bits, std::size_t after);

void xor_into(Bits& target, const Bits& source);

std::vector<std::size_t> set_columns(const Bits& bits);

Equation to_equation(Algebra backend, const Bits& bits);

Bits to_bits(const Equation& equation);

/// @brief A row with each column set independently with probability `1 / density`.
Bits random_bits(TestRandom& random, std::size_t num_vars, std::size_t density);

/// @brief Translate a partial-reduction sentinel into a `ReduceResult`.
///
/// `leading` is `std::nullopt` for @f$\mathbf{0}@f$, otherwise the leading
/// column, where `num_vars` -- the constant column index -- means
/// @f$\mathbf{1}@f$.
ReduceResult expected_result(const std::optional<std::size_t>& leading, std::size_t num_vars);

/// @brief A deliberately naive row-echelon matrix, used as an oracle.
///
/// Written the obvious way rather than the fast way -- in particular
/// `add_row_get_index` walks every column from 0 to `num_vars` one at a time,
/// rather than jumping between set columns -- so that `RowEchelonMatrix`, which
/// is free to be clever, can be checked against it for identical results and
/// identical insertion indices.
class ReferenceMatrix {
public:
    explicit ReferenceMatrix(std::size_t num_vars)
        : num_vars_(num_vars), pivot_(num_vars + 1, false), column_to_row_(num_vars + 1, 0) {}

    std::size_t rows() const { return rows_.size(); }

    /// @brief Row `index`, in row-echelon order.
    const Bits& row(std::size_t index) const { return rows_[index]; }

    bool is_pivot(std::size_t column) const { return pivot_[column]; }

    /// @brief Index of the row whose leading column is `column`.
    std::size_t row_for_pivot_column(std::size_t column) const { return column_to_row_[column]; }

    std::optional<std::size_t> add_row_get_index(const Bits& v);

    void reduce(Bits& v) const;

    /// @brief Reduce, reporting through a sentinel: `std::nullopt` for the true
    /// equation, otherwise the leading column, where `num_vars` means false.
    std::optional<std::size_t> partial_reduce(Bits& v) const;

    Bits pop_first_row();

    void pop_last_row();

private:
    std::size_t num_vars_;
    std::vector<Bits> rows_;
    std::vector<bool> pivot_;
    std::vector<std::size_t> column_to_row_;
};

} // namespace xorcle
