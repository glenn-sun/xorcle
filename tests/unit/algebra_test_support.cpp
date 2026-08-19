#include "algebra_test_support.h"

#include <doctest/doctest.h>

#include <optional>
#include <span>

namespace xorcle {

Equation make_equation_with(EquationRepr repr, std::size_t num_vars,
                            std::initializer_list<std::size_t> bits) {
    const std::vector<std::size_t> listed(bits);
    return Equation::from_bits(repr, num_vars, std::span<const std::size_t>(listed));
}

std::vector<std::size_t> bits_of(const Equation& equation) {
    std::vector<std::size_t> out;
    equation.bits(out);
    return out;
}

void check_echelon_invariant(const RowEchelonMatrix& matrix) {
    const ColumnSet& pivots = matrix.pivot_columns();
    REQUIRE(pivots.size() == matrix.cols());
    REQUIRE(pivots.count() == matrix.rows());

    std::optional<std::size_t> previous_pivot;
    for (std::size_t index = 0; index < matrix.rows(); ++index) {
        const Equation& row = matrix.row(index);
        REQUIRE(row.num_bits() == matrix.cols());
        REQUIRE(row.repr() == matrix.repr());

        const std::optional<std::size_t> leading = row.leading_bit();
        REQUIRE(leading.has_value());
        REQUIRE(*leading < matrix.cols());
        if (previous_pivot) {
            REQUIRE(*previous_pivot < *leading);
        }
        REQUIRE(pivots.test(*leading));
        previous_pivot = leading;
    }

    // Row iteration must agree with indexed access.
    std::size_t seen = 0;
    for (const Equation& row : matrix) {
        REQUIRE(row == matrix.row(seen));
        ++seen;
    }
    REQUIRE(seen == matrix.rows());
}

std::size_t TestRandom::next(std::size_t bound) noexcept {
    // splitmix64
    state_ += 0x9e3779b97f4a7c15ull;
    std::uint64_t z = state_;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    z ^= z >> 31;
    return bound == 0 ? 0 : static_cast<std::size_t>(z % bound);
}

} // namespace xorcle
