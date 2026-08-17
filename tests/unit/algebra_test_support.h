#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <vector>

#include "algebra/algebra.h"
#include "algebra/equation.h"
#include "algebra/row_echelon_matrix.h"

namespace xorcle {

/// @brief Tag selecting the dense backend in a `TEST_CASE_TEMPLATE`.
struct DenseTag {
    static constexpr Algebra value = Algebra::dense;
};

/// @brief Tag selecting the sparse backend in a `TEST_CASE_TEMPLATE`.
struct SparseTag {
    static constexpr Algebra value = Algebra::sparse;
};

/// @brief Build an equation with exactly the listed columns set, where column
/// `num_vars` is the constant column.
Equation make_equation_with(Algebra backend, std::size_t num_vars,
                            std::initializer_list<std::size_t> columns);

/// @brief The set columns of an equation, ascending.
std::vector<std::size_t> columns_of(const Equation& equation);

/// @brief Assert every representation invariant of a row-echelon matrix.
///
/// This mirrors the backend-internal `debug_assert_valid_representation()` but
/// runs through the public interface, so it holds the matrix to its stated
/// invariants in release builds too.
void check_echelon_invariant(const RowEchelonMatrix& matrix);

/// @brief A small deterministic generator, so failures are reproducible.
class TestRandom {
public:
    explicit TestRandom(std::uint64_t seed) noexcept : state_(seed | 1u) {}

    /// @brief Next value in `[0, bound)`.
    std::size_t next(std::size_t bound) noexcept;

    /// @brief A coin flip that comes up true with probability `1 / odds`.
    bool chance(std::size_t odds) noexcept { return next(odds) == 0; }

private:
    std::uint64_t state_;
};

} // namespace xorcle
