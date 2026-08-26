#include <doctest/doctest.h>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "algebra/equation.h"
#include "algebra/equation_repr.h"
#include "algebra/row_echelon_matrix.h"
#include "algebra_test_support.h"
#include "core/clause_id.h"
#include "core/trail.h"
#include "core/trail_basis.h"

namespace xorcle {

namespace {

/// @brief A reference count of how many levels a clause lets you discard,
/// derived from the row's set columns rather than from its leading column.
std::size_t removable_levels_reference(const TrailBasisSpace& basis, const Trail& trail) {
    REQUIRE(basis.rows() > 0);
    if (basis.rows() == 1) {
        return trail.size() - 1;
    }

    const std::vector<std::size_t> second_row = bits_of(basis.row(1));
    std::size_t removable_levels = 1;
    while (removable_levels < trail.size()) {
        const std::size_t earlier_trail_rows =
            trail[trail.size() - 1 - removable_levels].span().rows();
        const std::size_t columns_after_level = trail.span().rows() - earlier_trail_rows;

        bool second_row_suffix_any = false;
        for (const std::size_t col_idx : second_row) {
            if (col_idx < columns_after_level) {
                second_row_suffix_any = true;
                break;
            }
        }
        if (second_row_suffix_any) {
            return removable_levels - 1;
        }
        ++removable_levels;
    }
    return removable_levels - 1;
}

/// @brief Check `asserting_level()` against the reference arithmetic and against
/// the decision level of the trail step row 1 points at.
void check_asserting_level(const TrailBasisSpace& basis, const Trail& trail,
                           const std::vector<std::size_t>& level_of_trail_step) {
    const std::size_t asserting_level = basis.asserting_level();
    CHECK(asserting_level <= trail.level());
    CHECK(asserting_level == trail.level() - removable_levels_reference(basis, trail));

    // The independent derivation: column `c` is trail step `m - 1 - c`, so the
    // clause asserts at the level that step was learned at.
    if (basis.rows() == 1) {
        CHECK(asserting_level == 0);
    } else {
        const std::optional<std::size_t> leading = basis.row(1).leading_bit();
        REQUIRE(leading.has_value());
        const std::size_t trail_step = trail.span().rows() - *leading - 1;
        REQUIRE(trail_step < level_of_trail_step.size());
        CHECK(asserting_level == level_of_trail_step[trail_step]);
    }
}

/// @brief The literal block distance read straight off the definition.
///
/// Enumerates the subspace, intersects it with each prefix of the trail, and
/// counts the levels the dimension jumps in. Nothing here consults a pivot.
std::size_t literal_block_distance_reference(const TrailBasisSpace& basis, const Trail& trail,
                                             const std::vector<std::size_t>& level_of_trail_step) {
    const std::size_t cols = basis.cols();
    REQUIRE(basis.rows() <= 8);
    REQUIRE(cols <= 32);

    std::vector<std::uint32_t> subspace;
    for (std::uint32_t combination = 0; combination < (std::uint32_t{1} << basis.rows());
         ++combination) {
        std::uint32_t vector = 0;
        for (std::size_t row_idx = 0; row_idx < basis.rows(); ++row_idx) {
            if (((combination >> row_idx) & 1u) != 0u) {
                for (const std::size_t col_idx : bits_of(basis.row(row_idx))) {
                    vector ^= std::uint32_t{1} << col_idx;
                }
            }
        }
        subspace.push_back(vector);
    }

    // Trail step `t` is column `cols - 1 - t`, so the span of the first `i`
    // trail equations is the span of the columns at or above `cols - i`.
    const auto dimension_of_intersection = [&](std::size_t prefix) {
        const std::uint32_t below = prefix == cols
                                        ? 0u
                                        : (std::uint32_t{1} << (cols - prefix)) - 1u;
        std::size_t members = 0;
        for (const std::uint32_t vector : subspace) {
            if ((vector & below) == 0u) {
                ++members;
            }
        }
        return static_cast<std::size_t>(std::countr_zero(members));
    };

    std::vector<bool> level_seen(trail.size(), false);
    std::size_t blocks = 0;
    std::size_t previous_dimension = 0;
    for (std::size_t prefix = 1; prefix <= cols; ++prefix) {
        const std::size_t dimension = dimension_of_intersection(prefix);
        if (dimension > previous_dimension) {
            const std::size_t level = level_of_trail_step[prefix - 1];
            if (!level_seen[level]) {
                level_seen[level] = true;
                ++blocks;
            }
        }
        previous_dimension = dimension;
    }
    return blocks;
}

/// @brief A trail with a hand-checkable level structure.
///
/// Eight learned equations over four decision levels, all unit and on distinct
/// variables, so the trail span is exactly what was learned and `order_map()` is
/// the identity:
///
/// | trail step | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 |
/// |---|---|---|---|---|---|---|---|---|
/// | level | 0 | 1 | 1 | 2 | 2 | 3 | 3 | 3 |
/// | trail-basis column | 7 | 6 | 5 | 4 | 3 | 2 | 1 | 0 |
///
/// So `trail[k].span().rows()` is 1, 3, 5, 8 for k = 0, 1, 2, 3.
Trail make_levelled_trail(EquationRepr repr) {
    Trail trail(repr, 8);
    trail.learn(make_equation_with(repr, 8, {0, 8}), ClauseId{0}); // step 0, level 0
    trail.learn(make_equation_with(repr, 8, {1}), std::nullopt); // step 1, level 1
    trail.learn(make_equation_with(repr, 8, {2, 8}), ClauseId{1}); // step 2, level 1
    trail.learn(make_equation_with(repr, 8, {3}), std::nullopt); // step 3, level 2
    trail.learn(make_equation_with(repr, 8, {4, 8}), ClauseId{2}); // step 4, level 2
    trail.learn(make_equation_with(repr, 8, {5}), std::nullopt); // step 5, level 3
    trail.learn(make_equation_with(repr, 8, {6, 8}), ClauseId{3}); // step 6, level 3
    trail.learn(make_equation_with(repr, 8, {7}), ClauseId{4}); // step 7, level 3
    return trail;
}

/// @brief The per-trail-step levels of `make_levelled_trail()`.
const std::vector<std::size_t>& levelled_trail_step_levels() {
    static const std::vector<std::size_t> levels{0, 1, 1, 2, 2, 3, 3, 3};
    return levels;
}

/// @brief Build a trail-basis space by adding, in order, a row with each listed
/// set of columns.
TrailBasisSpace make_basis(const Trail& trail,
                           const std::vector<std::vector<std::size_t>>& rows) {
    TrailBasisSpace basis(trail);
    for (const std::vector<std::size_t>& col_idxs : rows) {
        Equation row = Equation::with_num_bits(trail.repr(), basis.cols());
        row.assign_bits(std::span<const std::size_t>(col_idxs));
        basis.add_row(row);
    }
    return basis;
}

/// @brief Whether two row spaces span the same subspace.
bool spans_equal(const RowSpace& lhs, const RowSpace& rhs) {
    if (lhs.rows() != rhs.rows()) {
        return false;
    }
    for (const Equation& row : lhs) {
        Equation reduced = row;
        rhs.reduce(reduced);
        if (!reduced.is_eq_true()) {
            return false;
        }
    }
    return true;
}

/// @brief A random trail together with the decision level of each of its steps.
struct RandomTrail {
    Trail trail;
    std::vector<std::size_t> level_of_trail_step;
};

/// @brief A random trail of unit equations on distinct variables.
RandomTrail make_random_trail(EquationRepr repr, TestRandom& rng, std::size_t num_vars) {
    std::vector<std::size_t> variables(num_vars);
    for (std::size_t i = 0; i < num_vars; ++i) {
        variables[i] = i;
    }
    for (std::size_t i = num_vars; i-- > 1;) {
        std::swap(variables[i], variables[rng.next(i + 1)]);
    }

    Trail trail(repr, num_vars);
    std::vector<std::size_t> level_of_trail_step;
    for (std::size_t step = 0; step < num_vars; ++step) {
        // Every equation is a unit on a variable no earlier step touched, so the
        // span never absorbs one and each step really does add a row.
        std::vector<std::size_t> col_idxs{variables[step]};
        if (rng.chance(2)) {
            col_idxs.push_back(num_vars);
        }
        Equation eqn(repr, num_vars);
        eqn.assign_bits(std::span<const std::size_t>(col_idxs));

        const bool decide = rng.chance(2);
        trail.learn(eqn, decide ? std::optional<ClauseId>{} : std::optional<ClauseId>{step});
        level_of_trail_step.push_back(trail.level());
    }
    return RandomTrail{std::move(trail), std::move(level_of_trail_step)};
}

} // namespace

TEST_CASE_TEMPLATE("trail basis: bit-count bookkeeping", Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    const Trail trail = make_levelled_trail(repr);
    const TrailBasisSpace basis(trail);

    CHECK(basis.cols() == trail.span().rows());
    CHECK(basis.rows() == 0);
    CHECK(basis.repr() == repr);

    // The last column is the oldest trail row's coordinate, so a row set there
    // alone is an ordinary basis row.
    const TrailBasisSpace oldest_only = make_basis(trail, {{7}});
    REQUIRE(oldest_only.rows() == 1);
    CHECK(bits_of(oldest_only.row(0)) == std::vector<std::size_t>{7});
    CHECK(oldest_only.to_RowSpace().rows() == 1);
    check_echelon_invariant(oldest_only);
}

TEST_CASE_TEMPLATE("trail basis: an empty trail span gives a zero-column basis", Tag, DenseTag,
                   SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    const Trail trail(repr, 4);
    const TrailBasisSpace basis(trail);

    CHECK(trail.span().rows() == 0);
    CHECK(basis.rows() == 0);
    CHECK(basis.cols() == 0);
}

TEST_CASE_TEMPLATE("trail basis: from_RowSpace maps trail step t to column m-1-t", Tag, DenseTag,
                   SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    Trail trail(repr, 4);

    // Learned leading columns 2, 0, 3, so row-echelon order and trail order
    // disagree.
    trail.learn(make_equation_with(repr, 4, {2}), ClauseId{0});
    trail.learn(make_equation_with(repr, 4, {0, 4}), ClauseId{1});
    trail.learn(make_equation_with(repr, 4, {3, 4}), ClauseId{2});
    REQUIRE(trail.order_map() == std::vector<std::size_t>{1, 0, 2});

    const std::size_t span_rows = trail.span().rows();
    for (std::size_t trail_step = 0; trail_step < span_rows; ++trail_step) {
        RowSpace one_row(repr, trail.num_vars());
        one_row.add_row(trail.span().row(trail.order_map()[trail_step]));

        const TrailBasisSpace basis = TrailBasisSpace::from_RowSpace(one_row, trail);
        REQUIRE(basis.rows() == 1);
        const std::vector<std::size_t> expected{span_rows - trail_step - 1};
        CHECK(bits_of(basis.row(0)) == expected);

        CHECK(spans_equal(basis.to_RowSpace(), one_row));
    }
}

TEST_CASE_TEMPLATE("trail basis: from_RowSpace rewrites a combination of trail rows", Tag,
                   DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    const Trail trail = make_levelled_trail(repr);
    REQUIRE(trail.span().rows() == 8);

    // Steps 1 and 6 together, i.e. columns 8 - 1 - 1 = 6 and 8 - 6 - 1 = 1.
    Equation combination = trail.span().row(trail.order_map()[1]);
    combination.plus_assign(trail.span().row(trail.order_map()[6]));

    RowSpace row_space(repr, trail.num_vars());
    row_space.add_row(combination);

    const TrailBasisSpace basis = TrailBasisSpace::from_RowSpace(row_space, trail);
    REQUIRE(basis.rows() == 1);
    CHECK(bits_of(basis.row(0)) == std::vector<std::size_t>{1, 6});
    check_echelon_invariant(basis);
    CHECK(spans_equal(basis.to_RowSpace(), row_space));
}

TEST_CASE_TEMPLATE("trail basis: from_RowSpace of the whole span is the full coordinate basis",
                   Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    const Trail trail = make_levelled_trail(repr);

    RowSpace whole_span(repr, trail.num_vars());
    for (const Equation& row : trail.span()) {
        whole_span.add_row(row);
    }

    const TrailBasisSpace basis = TrailBasisSpace::from_RowSpace(whole_span, trail);
    REQUIRE(basis.rows() == trail.span().rows());
    for (std::size_t index = 0; index < basis.rows(); ++index) {
        CHECK(bits_of(basis.row(index)) == std::vector<std::size_t>{index});
    }
    CHECK(spans_equal(basis.to_RowSpace(), whole_span));
}

TEST_CASE_TEMPLATE("trail basis: asserting level of a unit clause is the root", Tag,
                   DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    const Trail trail = make_levelled_trail(repr);
    REQUIRE(trail.level() == 3);

    // A single row converts back to a unit clause, so it asserts at the root
    // whatever it is made of. The reference counts levels discarded instead,
    // which is 3 here -- the opposite end of the same range.
    for (const std::size_t col_idx : {std::size_t{0}, std::size_t{3}, std::size_t{7}}) {
        const TrailBasisSpace basis = make_basis(trail, {{col_idx}});
        REQUIRE(basis.rows() == 1);
        CHECK(basis.asserting_level() == 0);
        check_asserting_level(basis, trail, levelled_trail_step_levels());
    }
}

TEST_CASE_TEMPLATE("trail basis: asserting level with a hand-computed answer", Tag, DenseTag,
                   SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    const Trail trail = make_levelled_trail(repr);
    REQUIRE(trail.level() == 3);

    // `make_levelled_trail()` gives level column counts (equations learned strictly
    // after level k) of 0, 3, 5, 7 for k = 3, 2, 1, 0, so the leading column of
    // row 1 lands as follows:
    //
    //   columns 0, 1, 2 -> trail steps 7, 6, 5 -> level 3
    //   columns 3, 4    -> trail steps 4, 3    -> level 2
    //   columns 5, 6    -> trail steps 2, 1    -> level 1
    //   column  7       -> trail step  0       -> level 0
    struct Case {
        std::vector<std::vector<std::size_t>> rows;
        std::size_t expected;
    };
    const std::vector<Case> cases{
        // Row 1 touches the most recent level, so nothing is discardable.
        {{{0}, {1}}, 3},
        {{{0}, {2}}, 3},
        {{{0, 4}, {2, 7}}, 3},
        // Row 1 reaches back to level 2, so level 3 goes.
        {{{0}, {3}}, 2},
        {{{1}, {4}}, 2},
        // Only row 1 matters: the later rows reach further back, hence to
        // lower levels, and cannot raise the answer.
        {{{0}, {3}, {6}}, 2},
        {{{0}, {3}, {5}, {7}}, 2},
        // Row 1 reaches back to level 1.
        {{{0}, {5}}, 1},
        {{{4}, {6}}, 1},
        // Row 1 reaches all the way to the root equation.
        {{{3}, {7}}, 0},
        {{{0}, {7}}, 0},
    };

    for (const Case& test_case : cases) {
        const TrailBasisSpace basis = make_basis(trail, test_case.rows);
        REQUIRE(basis.rows() == test_case.rows.size());
        CHECK(basis.asserting_level() == test_case.expected);
        check_asserting_level(basis, trail, levelled_trail_step_levels());
    }
}

TEST_CASE_TEMPLATE("trail basis: asserting level on a single-level trail is the root", Tag,
                   DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    Trail trail(repr, 4);
    trail.learn(make_equation_with(repr, 4, {0}), ClauseId{0});
    trail.learn(make_equation_with(repr, 4, {1, 4}), ClauseId{1});
    REQUIRE(trail.level() == 0);

    // With no decisions there is nothing to discard and nothing to keep, and
    // both ends of the range collapse onto the root.
    const std::vector<std::size_t> step_levels{0, 0};

    const TrailBasisSpace one_row = make_basis(trail, {{0}});
    CHECK(one_row.asserting_level() == 0);
    check_asserting_level(one_row, trail, step_levels);

    const TrailBasisSpace two_rows = make_basis(trail, {{0}, {1}});
    REQUIRE(two_rows.rows() == 2);
    CHECK(two_rows.asserting_level() == 0);
    check_asserting_level(two_rows, trail, step_levels);
}

TEST_CASE_TEMPLATE("trail basis: asserting level agrees with the reference arithmetic on random input",
                   Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    TestRandom rng(987654321u);

    for (std::size_t iteration = 0; iteration < 400; ++iteration) {
        const std::size_t num_vars = 1 + rng.next(10);
        const RandomTrail random_trail = make_random_trail(repr, rng, num_vars);
        const Trail& trail = random_trail.trail;
        REQUIRE(trail.span().rows() == num_vars);

        TrailBasisSpace basis(trail);
        const std::size_t attempts = 1 + rng.next(4);
        for (std::size_t attempt = 0; attempt < attempts; ++attempt) {
            std::vector<std::size_t> col_idxs;
            for (std::size_t col_idx = 0; col_idx < basis.cols(); ++col_idx) {
                if (rng.chance(3)) {
                    col_idxs.push_back(col_idx);
                }
            }
            if (col_idxs.empty()) {
                continue;
            }
            Equation row = Equation::with_num_bits(repr, basis.cols());
            row.assign_bits(std::span<const std::size_t>(col_idxs));
            basis.add_row(row);
        }
        if (basis.rows() == 0) {
            continue;
        }

        check_echelon_invariant(basis);
        check_asserting_level(basis, trail, random_trail.level_of_trail_step);
    }
}

TEST_CASE_TEMPLATE("trail basis: from_RowSpace and to_RowSpace round-trip on random input", Tag,
                   DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    TestRandom rng(123456789u);

    for (std::size_t iteration = 0; iteration < 200; ++iteration) {
        const std::size_t num_vars = 1 + rng.next(10);
        const Trail trail = make_random_trail(repr, rng, num_vars).trail;

        // Random combinations of trail rows, so every row lies in the trail
        // span, as `from_RowSpace()` requires.
        RowSpace row_space(repr, trail.num_vars());
        const std::size_t attempts = 1 + rng.next(5);
        for (std::size_t attempt = 0; attempt < attempts; ++attempt) {
            Equation combination(repr, trail.num_vars());
            for (const Equation& span_row : trail.span()) {
                if (rng.chance(2)) {
                    combination.plus_assign(span_row);
                }
            }
            if (!combination.is_eq_true()) {
                row_space.add_row(combination);
            }
        }

        const TrailBasisSpace basis = TrailBasisSpace::from_RowSpace(row_space, trail);
        check_echelon_invariant(basis);
        CHECK(basis.rows() == row_space.rows());
        CHECK(spans_equal(basis.to_RowSpace(), row_space));

        // And the other direction: coordinates out, coordinates back in.
        const TrailBasisSpace again = TrailBasisSpace::from_RowSpace(basis.to_RowSpace(), trail);
        REQUIRE(again.rows() == basis.rows());
        for (std::size_t index = 0; index < basis.rows(); ++index) {
            CHECK(bits_of(again.row(index)) == bits_of(basis.row(index)));
        }
    }
}

TEST_CASE_TEMPLATE("trail basis: literal block distance with hand-computed answers", Tag, DenseTag,
                   SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    const Trail trail = make_levelled_trail(repr);
    const std::vector<std::size_t>& levels = levelled_trail_step_levels();

    // Columns 0, 1, 2 are level 3; 3, 4 are level 2; 5, 6 are level 1; 7 is the
    // root. A row's pivot is what counts, not every column it touches.
    SUBCASE("rows inside one level count once") {
        const TrailBasisSpace basis = make_basis(trail, {{0}, {1}, {2}});
        CHECK(basis.literal_block_distance() == 1);
        CHECK(basis.literal_block_distance() ==
              literal_block_distance_reference(basis, trail, levels));
    }

    SUBCASE("one row per level counts every level") {
        const TrailBasisSpace basis = make_basis(trail, {{0}, {3}, {5}, {7}});
        CHECK(basis.literal_block_distance() == 4);
        CHECK(basis.literal_block_distance() ==
              literal_block_distance_reference(basis, trail, levels));
    }

    SUBCASE("a column below the pivot does not add its level") {
        // Pivots 0 and 2 are both level 3, though column 3 is level 2.
        const TrailBasisSpace basis = make_basis(trail, {{0, 1}, {2, 3}});
        CHECK(basis.literal_block_distance() == 1);
        CHECK(basis.literal_block_distance() ==
              literal_block_distance_reference(basis, trail, levels));
    }

    SUBCASE("a single row reaches only its own level") {
        const TrailBasisSpace basis = make_basis(trail, {{4, 6, 7}});
        CHECK(basis.literal_block_distance() == 1);
        CHECK(basis.literal_block_distance() ==
              literal_block_distance_reference(basis, trail, levels));
    }

    SUBCASE("the whole coordinate basis reaches every level") {
        const TrailBasisSpace basis = make_basis(trail, {{0}, {1}, {2}, {3}, {4}, {5}, {6}, {7}});
        CHECK(basis.literal_block_distance() == trail.size());
    }
}

TEST_CASE_TEMPLATE("trail basis: literal block distance agrees with the definition on random input",
                   Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    TestRandom rng(24681357u);

    for (std::size_t iteration = 0; iteration < 400; ++iteration) {
        const std::size_t num_vars = 1 + rng.next(10);
        const RandomTrail random_trail = make_random_trail(repr, rng, num_vars);
        const Trail& trail = random_trail.trail;
        REQUIRE(trail.span().rows() == num_vars);

        TrailBasisSpace basis(trail);
        const std::size_t attempts = 1 + rng.next(4);
        for (std::size_t attempt = 0; attempt < attempts; ++attempt) {
            std::vector<std::size_t> col_idxs;
            for (std::size_t col_idx = 0; col_idx < basis.cols(); ++col_idx) {
                if (rng.chance(3)) {
                    col_idxs.push_back(col_idx);
                }
            }
            if (col_idxs.empty()) {
                continue;
            }
            Equation row = Equation::with_num_bits(repr, basis.cols());
            row.assign_bits(std::span<const std::size_t>(col_idxs));
            basis.add_row(row);
        }
        if (basis.rows() == 0) {
            continue;
        }

        const std::size_t lbd = basis.literal_block_distance();
        CHECK(lbd == literal_block_distance_reference(basis, trail,
                                                      random_trail.level_of_trail_step));
        CHECK(lbd >= 1);
        CHECK(lbd <= basis.rows());
        CHECK(lbd <= trail.size());
    }
}

} // namespace xorcle
