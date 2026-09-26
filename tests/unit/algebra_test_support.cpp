#include "algebra_test_support.h"

#include <doctest/doctest.h>

#include <cassert>
#include <optional>
#include <span>

namespace xorcle {

Equation make_equation_with(EquationRepr repr, std::size_t num_vars,
                            std::initializer_list<std::size_t> bits) {
    const std::vector<std::size_t> listed(bits);
    return Equation::from_bits(repr, num_vars, std::span<const std::size_t>(listed));
}

Equation make_equation_with_num_bits(EquationRepr repr, std::size_t num_bits,
                                     std::initializer_list<std::size_t> bits) {
    const std::vector<std::size_t> listed(bits);
    Equation out = Equation::with_num_bits(repr, num_bits);
    out.assign_bits(std::span<const std::size_t>(listed));
    return out;
}

std::vector<std::vector<std::size_t>> rows_of(const RowEchelonMatrix& matrix) {
    std::vector<std::vector<std::size_t>> rows;
    for (const Equation& row : matrix) {
        rows.push_back(row.bits());
    }
    return rows;
}

std::vector<std::size_t> bits_of(const Equation& equation) {
    return equation.bits();
}

void check_echelon_invariant(const RowEchelonMatrix& matrix) {
    const IndexSet& pivots = matrix.pivot_col_idxs();
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
        REQUIRE(matrix.pivot_col_idx(index) == *leading);
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
    assert(bound > 0 && "TestRandom::next needs a positive bound");

    // splitmix64
    state_ += 0x9e3779b97f4a7c15ull;
    std::uint64_t z = state_;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    z ^= z >> 31;
    return static_cast<std::size_t>(z % bound);
}

} // namespace xorcle
