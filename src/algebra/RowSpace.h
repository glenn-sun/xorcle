#pragma once

#include <cstddef>
#include <string>

#include "PrintMode.h"
#include "algebra/REFMatrix.h"
#include "bit/vector.h"

namespace algebra {

/// @brief A subspace of the space of linear equations over GF(2),
/// represented as a basis in row-echelon form.
///
/// Each column corresponds to a variable in the original order.
class RowSpace : public REFMatrix {
public:
    using REFMatrix::REFMatrix;

    /// @brief Get the unique solution to the system of equations represented by this matrix.
    /// @pre The matrix is full-rank and does not contain the false equation.
    bit::vector<> get_unique_solution() const;
};

/// @brief A clause in its annihilator representation, as the conjunction
/// of linear equations whose solutions are where the clause is false.
///
/// Example: The clause [x1 + x2 = 1] v [x3 = 0] is represented as
/// [x1 + x2 = 0] ^ [x3 = 1], i.e. [[1, 1, 0, 0], [0, 0, 1, 1]].
class Clause : public RowSpace {
public:
    /// @brief Index of the first watched equation.
    std::size_t watched0 = 0;

    /// @brief Index of the second watched equation.
    std::size_t watched1 = 1;

    /// @brief Index of a known-true equation to skip recomputation.
    std::size_t true_witness = 0;

    /// @brief Convert the clause to a printable string.
    std::string to_string(PrintMode mode = PrintMode()) const;

    /// @brief Construct a clause for a given variable count.
    Clause(std::size_t num_vars) : RowSpace(num_vars) {}

    /// @brief Construct a clause from an existing row space.
    Clause(const RowSpace& rs) : RowSpace(rs) {}
};
} // namespace algebra
