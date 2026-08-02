#include "algebra_test_support.h"

#include <doctest/doctest.h>

#include <optional>
#include <span>

namespace xorcle {

Equation make_equation_with(Algebra backend, std::size_t num_vars,
                            std::initializer_list<std::size_t> columns) {
    const std::vector<std::size_t> listed(columns);
    return Equation::from_columns(backend, num_vars, std::span<const std::size_t>(listed));
}

std::vector<std::size_t> columns_of(const Equation& equation) {
    std::vector<std::size_t> out;
    equation.columns(out);
    return out;
}

void check_echelon_invariant(const RowEchelonMatrix& matrix) {
    const Bitmap& pivots = matrix.pivot_columns();
    REQUIRE(pivots.size() == matrix.cols());
    REQUIRE(pivots.count() == matrix.rows());

    std::optional<std::size_t> previous_pivot;
    for (std::size_t index = 0; index < matrix.rows(); ++index) {
        const Equation& row = matrix.row(index);
        REQUIRE(row.size() == matrix.cols());
        REQUIRE(row.backend() == matrix.backend());

        const std::optional<std::size_t> leading = row.leading_column();
        REQUIRE(leading.has_value());
        REQUIRE(*leading < matrix.cols());
        if (previous_pivot) {
            REQUIRE(*previous_pivot < *leading);
        }
        REQUIRE(pivots.test(*leading));
        REQUIRE(matrix.row_for_pivot_column(*leading) == index);
        previous_pivot = leading;
    }

    pivots.for_each_set([&](std::size_t pivot_column) {
        const std::size_t index = matrix.row_for_pivot_column(pivot_column);
        REQUIRE(index < matrix.rows());
        REQUIRE(matrix.row(index).leading_column() == std::optional<std::size_t>(pivot_column));
    });

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
