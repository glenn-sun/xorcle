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

/// @brief First set column of `bits`, or `std::nullopt`.
std::optional<std::size_t> first_set(const Bits& bits);

/// @brief First set column of `bits` strictly after `after`, or `std::nullopt`.
std::optional<std::size_t> next_set(const Bits& bits, std::size_t after);

/// @brief Add `source` into `target` over GF(2).
void xor_into(Bits& target, const Bits& source);

/// @brief The set columns of `bits`, ascending.
std::vector<std::size_t> set_columns(const Bits& bits);

/// @brief Build an equation on `backend` with the same set columns as `bits`.
Equation to_equation(Algebra backend, const Bits& bits);

/// @brief Convert an equation back to a plain bit vector.
Bits to_bits(const Equation& equation);

/// @brief A row with each column set independently with probability `1 / density`.
Bits random_bits(TestRandom& random, std::size_t num_vars, std::size_t density);

/// @brief Translate the old tree's partial-reduction sentinel into a `ReduceResult`.
/// @param leading `std::nullopt` for @f$\mathbf{0}@f$, otherwise the leading
/// column, where `num_vars` meant @f$\mathbf{1}@f$.
/// @param num_vars The constant column index.
ReduceResult expected_result(const std::optional<std::size_t>& leading, std::size_t num_vars);

/// @brief A transcription of the old tree's `REFMatrix`, used as an oracle.
///
/// Deliberately written the way `REFMatrix.cpp` was -- in particular
/// `add_row_get_index` walks every column from 0 to `num_vars` calling
/// `element()`, rather than jumping between set columns -- so that the new
/// implementation's insertion indices can be checked against the old behaviour
/// exactly. `add_row_get_index` is the one operation the design note allowed to
/// change shape, on the condition that it produce identical results.
class ReferenceMatrix {
public:
    /// @brief Construct an empty reference matrix over `num_vars` variables.
    explicit ReferenceMatrix(std::size_t num_vars)
        : num_vars_(num_vars), pivot_(num_vars + 1, false), column_to_row_(num_vars + 1, 0) {}

    /// @brief Number of stored rows.
    std::size_t rows() const { return rows_.size(); }

    /// @brief Row `index`, in row-echelon order.
    const Bits& row(std::size_t index) const { return rows_[index]; }

    /// @brief Whether `column` is a pivot column.
    bool is_pivot(std::size_t column) const { return pivot_[column]; }

    /// @brief Index of the row whose leading column is `column`.
    std::size_t row_for_pivot_column(std::size_t column) const { return column_to_row_[column]; }

    /// @brief The old `REFMatrix::add_row_get_index`, transcribed.
    std::optional<std::size_t> add_row_get_index(const Bits& v);

    /// @brief The old `REFMatrix::reduce`, transcribed.
    void reduce(Bits& v) const;

    /// @brief The old `REFMatrix::partial_reduce`, in its sentinel convention.
    /// @return `std::nullopt` for the true equation, otherwise the leading
    /// column, where `num_vars` means the false equation.
    std::optional<std::size_t> partial_reduce(Bits& v) const;

    /// @brief The old `REFMatrix::pop_first_row`, transcribed.
    Bits pop_first_row();

    /// @brief The old `REFMatrix::pop_last_row`, transcribed.
    void pop_last_row();

private:
    std::size_t num_vars_;
    std::vector<Bits> rows_;
    std::vector<bool> pivot_;
    std::vector<std::size_t> column_to_row_;
};

} // namespace xorcle
