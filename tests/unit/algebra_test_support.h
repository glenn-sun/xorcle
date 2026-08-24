#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <vector>

#include "algebra/equation.h"
#include "algebra/equation_repr.h"
#include "algebra/row_echelon_matrix.h"

namespace xorcle {

/// @brief Tag selecting the dense representation in a `TEST_CASE_TEMPLATE`.
struct DenseTag {
    static constexpr EquationRepr value = EquationRepr::dense;
};

/// @brief Tag selecting the sparse representation in a `TEST_CASE_TEMPLATE`.
struct SparseTag {
    static constexpr EquationRepr value = EquationRepr::sparse;
};

/// @brief Build an equation with exactly the listed bits set, where bit
/// `num_vars` is the constant bit.
Equation make_equation_with(EquationRepr repr, std::size_t num_vars,
                            std::initializer_list<std::size_t> bits);

/// @brief Build a coefficient vector of `num_bits` bits with exactly the
/// listed bits set.
Equation make_equation_with_num_bits(EquationRepr repr, std::size_t num_bits,
                                     std::initializer_list<std::size_t> bits);

/// @brief The set bits of an equation, ascending.
std::vector<std::size_t> bits_of(const Equation& equation);

/// @brief Assert every representation invariant of a row-echelon matrix,
/// through its public interface, in every build.
void check_echelon_invariant(const RowEchelonMatrix& matrix);

/// @brief A small deterministic generator, so failures are reproducible.
class TestRandom {
public:
    explicit TestRandom(std::uint64_t seed) noexcept : state_(seed | 1u) {}

    /// @brief Next value in `[0, bound)`.
    /// @pre `bound > 0`.
    std::size_t next(std::size_t bound) noexcept;

    /// @brief A coin flip that comes up true with probability `1 / odds`.
    bool chance(std::size_t odds) noexcept { return next(odds) == 0; }

private:
    std::uint64_t state_;
};

} // namespace xorcle
