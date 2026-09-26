#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <vector>

#include "algebra/equation.h"
#include "algebra/reduce_result.h"
#include "algebra_test_support.h"
#include "core/clause_id.h"
#include "core/trail.h"

namespace xorcle {

namespace {

/// @brief The trail-order sequence of set columns, resolved through `trail_idx_to_row_idx()`.
std::vector<std::vector<std::size_t>> rows_in_trail_order(const Trail& trail) {
    std::vector<std::vector<std::size_t>> out;
    for (const std::size_t row_idx : trail.trail_idx_to_row_idx()) {
        out.push_back(bits_of(trail.span().row(row_idx)));
    }
    return out;
}

/// @brief An equation with each bit set independently with probability `1 / 3`.
Equation random_equation(TestRandom& random, EquationRepr repr, std::size_t num_vars) {
    std::vector<std::size_t> bits;
    for (std::size_t bit = 0; bit <= num_vars; ++bit) {
        if (random.chance(3)) {
            bits.push_back(bit);
        }
    }
    return Equation::from_bits(repr, num_vars, bits);
}

/// @brief The rows a trail holds, kept in trail order, where a learn at
/// `trail_idx` re-reduces every row from there on against the rows before it.
class ReferenceTrail {
public:
    void learn(const Equation& eqn, std::size_t trail_idx) {
        std::vector<Equation> above(rows_.begin() + static_cast<std::ptrdiff_t>(trail_idx),
                                    rows_.end());
        rows_.resize(trail_idx);
        rows_.push_back(reduced(eqn));
        for (const Equation& row : above) {
            rows_.push_back(reduced(row));
        }
    }

    std::vector<std::vector<std::size_t>> rows_in_trail_order() const {
        std::vector<std::vector<std::size_t>> out;
        for (const Equation& row : rows_) {
            out.push_back(bits_of(row));
        }
        return out;
    }

private:
    /// @brief `eqn` with every leading column an already stored row holds eliminated.
    Equation reduced(const Equation& eqn) const {
        Equation out = eqn;
        while (true) {
            const std::optional<std::size_t> leading_col_idx = out.leading_bit();
            if (!leading_col_idx) {
                return out;
            }
            const auto same_leading = [&](const Equation& row) {
                return row.leading_bit() == leading_col_idx;
            };
            const auto it = std::find_if(rows_.begin(), rows_.end(), same_leading);
            if (it == rows_.end()) {
                return out;
            }
            out.plus_assign(*it);
        }
    }

    std::vector<Equation> rows_;
};

} // namespace

TEST_CASE_TEMPLATE("trail: an empty trail", Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    const Trail trail(repr, 4);

    CHECK(trail.level() == 0);
    CHECK(trail.repr() == repr);
    CHECK(trail.num_vars() == 4);
    CHECK(trail.span().rows() == 0);
    CHECK(trail.span().cols() == 5);
    CHECK(trail.trail_idx_to_row_idx().empty());
    CHECK(trail.row_idx_to_trail_idx().empty());
    CHECK(trail.reasons().empty());
}

TEST_CASE_TEMPLATE("trail: propagations stay on the current level", Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    Trail trail(repr, 4);

    trail.learn(make_equation_with(repr, 4, {0, 4}), ClauseId{7}, trail.level());
    CHECK(trail.level() == 0);
    CHECK(trail.span().rows() == 1);
    REQUIRE(trail.reasons().size() == 1);
    CHECK(trail.reasons()[0] == std::optional<ClauseId>{7});

    trail.learn(make_equation_with(repr, 4, {1}), ClauseId{2}, trail.level());
    CHECK(trail.level() == 0);
    CHECK(trail.span().rows() == 2);
    CHECK(trail.reasons()[1] == std::optional<ClauseId>{2});
}

TEST_CASE_TEMPLATE("trail: decisions open a level", Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    Trail trail(repr, 4);

    trail.learn(make_equation_with(repr, 4, {0, 4}), ClauseId{7}, trail.level());
    trail.learn(make_equation_with(repr, 4, {1}), std::nullopt, trail.level() + 1);

    CHECK(trail.level() == 1);
    CHECK(trail.span().rows() == 2);
    CHECK(trail.reasons()[1] == std::nullopt);

    // Computing the asserting level reads `rows_at_level(k)` per level.
    CHECK(trail.rows_at_level(0) == 1);
    CHECK(trail.rows_at_level(1) == 2);
    CHECK(trail.level_of(0) == 0);

    // A level begins, in trail order, with its decision.
    CHECK(rows_in_trail_order(trail)[1] == std::vector<std::size_t>{1});

    trail.learn(make_equation_with(repr, 4, {2}), ClauseId{5}, trail.level());
    CHECK(trail.level() == 1);
    CHECK(trail.rows_at_level(0) == 1);
    CHECK(trail.rows_at_level(1) == 3);
}

TEST_CASE_TEMPLATE("trail: order maps are inverse permutations", Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    Trail trail(repr, 4);

    // Learned leading columns 2, 0, 3 in that order, so row-echelon order and
    // trail order genuinely disagree and the insertion shift is exercised.
    trail.learn(make_equation_with(repr, 4, {2}), ClauseId{0}, trail.level());
    CHECK(trail.trail_idx_to_row_idx() == std::vector<std::size_t>{0});
    CHECK(trail.row_idx_to_trail_idx() == std::vector<std::size_t>{0});

    trail.learn(make_equation_with(repr, 4, {0, 4}), ClauseId{1}, trail.level());
    CHECK(trail.trail_idx_to_row_idx() == std::vector<std::size_t>{1, 0});
    CHECK(trail.row_idx_to_trail_idx() == std::vector<std::size_t>{1, 0});

    trail.learn(make_equation_with(repr, 4, {3, 4}), ClauseId{2}, trail.level());
    CHECK(trail.trail_idx_to_row_idx() == std::vector<std::size_t>{1, 0, 2});
    CHECK(trail.row_idx_to_trail_idx() == std::vector<std::size_t>{1, 0, 2});

    // Every trail step resolves to the row it created, up to reduction by the
    // rows already present.
    const std::vector<std::vector<std::size_t>> expected{{2}, {0, 4}, {3, 4}};
    CHECK(rows_in_trail_order(trail) == expected);

    for (std::size_t t = 0; t < trail.trail_idx_to_row_idx().size(); ++t) {
        CHECK(trail.row_idx_to_trail_idx()[trail.trail_idx_to_row_idx()[t]] == t);
    }
    for (std::size_t r = 0; r < trail.row_idx_to_trail_idx().size(); ++r) {
        CHECK(trail.trail_idx_to_row_idx()[trail.row_idx_to_trail_idx()[r]] == r);
    }
    CHECK(trail.reasons().size() == trail.span().rows());
}

TEST_CASE_TEMPLATE("trail: learning a partially reduced equation", Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    Trail trail(repr, 4);

    trail.learn(make_equation_with(repr, 4, {0, 1}), ClauseId{0}, trail.level());

    // The solver always partially reduces before learning, which is what the
    // precondition on `learn()` asks for.
    Equation candidate = make_equation_with(repr, 4, {0, 2});
    const ReduceResult reduced = trail.span().partial_reduce(candidate);
    CHECK(reduced == ReduceResult::leading(1));
    CHECK(bits_of(candidate) == std::vector<std::size_t>{1, 2});
    trail.learn(candidate, ClauseId{1}, trail.level());

    CHECK(trail.span().rows() == 2);
    CHECK(trail.trail_idx_to_row_idx() == std::vector<std::size_t>{0, 1});
    const std::vector<std::vector<std::size_t>> expected{{0, 1}, {1, 2}};
    CHECK(rows_in_trail_order(trail) == expected);
}

TEST_CASE_TEMPLATE("trail: resize truncates trailing levels", Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    Trail trail(repr, 4);

    trail.learn(make_equation_with(repr, 4, {0, 4}), ClauseId{9}, trail.level());
    trail.learn(make_equation_with(repr, 4, {1}), std::nullopt, trail.level() + 1);
    trail.learn(make_equation_with(repr, 4, {2}), ClauseId{3}, trail.level());
    trail.learn(make_equation_with(repr, 4, {3}), std::nullopt, trail.level() + 1);
    REQUIRE(trail.level() == 2);
    REQUIRE(trail.span().rows() == 4);

    SUBCASE("backjump to an intermediate level") {
        const std::size_t asserting_level = 1;
        trail.resize(asserting_level + 1);
        CHECK(trail.level() == 1);
        CHECK(trail.span().rows() == 3);
        CHECK(trail.reasons().size() == 3);
    }

    SUBCASE("restart back to the root") {
        trail.resize(1);
        CHECK(trail.level() == 0);
        CHECK(trail.span().rows() == 1);
        const std::vector<std::optional<ClauseId>> expected_reasons{std::optional<ClauseId>{9}};
        CHECK(trail.reasons() == expected_reasons);
        CHECK(trail.trail_idx_to_row_idx() == std::vector<std::size_t>{0});
    }

    SUBCASE("truncating to the current level is a no-op") {
        trail.resize(trail.level() + 1);
        CHECK(trail.level() == 2);
        CHECK(trail.span().rows() == 4);
    }
}

TEST_CASE_TEMPLATE("trail: learning after a backjump reuses the restored level", Tag, DenseTag,
                   SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    Trail trail(repr, 4);

    trail.learn(make_equation_with(repr, 4, {0}), std::nullopt, trail.level() + 1);
    trail.learn(make_equation_with(repr, 4, {1}), std::nullopt, trail.level() + 1);
    REQUIRE(trail.level() == 2);

    trail.resize(1);
    trail.learn(make_equation_with(repr, 4, {2, 4}), ClauseId{4}, trail.level());

    CHECK(trail.level() == 0);
    CHECK(trail.span().rows() == 1);
    CHECK(bits_of(trail.span().row(0)) == std::vector<std::size_t>{2, 4});
    const std::vector<std::optional<ClauseId>> expected_reasons{std::optional<ClauseId>{4}};
    CHECK(trail.reasons() == expected_reasons);
}

TEST_CASE_TEMPLATE("trail: learning below the current level", Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    Trail trail(repr, 6);

    trail.learn(make_equation_with(repr, 6, {1}), std::nullopt, 1);
    trail.learn(make_equation_with(repr, 6, {2, 3}), std::nullopt, 2);

    // Reduced against the level-2 row this would become `x3 + x4 = 0`, which no
    // level-1 equation implies.
    trail.learn(make_equation_with(repr, 6, {2, 4}), ClauseId{0}, 1);

    REQUIRE(trail.level() == 2);
    REQUIRE(trail.span().rows() == 3);
    CHECK(trail.rows_at_level(1) == 2);
    CHECK(trail.level_of(0) == 1);
    CHECK(trail.level_of(1) == 1);
    CHECK(trail.level_of(2) == 2);
    CHECK(trail.reasons()[1] == std::optional<ClauseId>{0});

    // The span is unchanged as a subspace, and the level-2 element is the one
    // whose representative moved.
    const std::vector<std::vector<std::size_t>> expected{{1}, {2, 4}, {3, 4}};
    CHECK(rows_in_trail_order(trail) == expected);

    // Every level-1 row lies in the span of the level-1 equations, which is what
    // makes the trail-basis columns a reverse-topological order.
    trail.resize(2);
    CHECK(trail.level() == 1);
    CHECK(trail.span().rows() == 2);
    CHECK(rows_in_trail_order(trail) == std::vector<std::vector<std::size_t>>{{1}, {2, 4}});
}

TEST_CASE_TEMPLATE("trail: stored reason coordinates go when a row at or below them changes",
                   Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    Trail trail(repr, 6);
    trail.learn(make_equation_with(repr, 6, {0}), std::nullopt, 1);
    trail.learn(make_equation_with(repr, 6, {1}), ClauseId{0}, 1);
    trail.learn(make_equation_with(repr, 6, {2}), std::nullopt, 2);
    trail.learn(make_equation_with(repr, 6, {3}), ClauseId{1}, 2);
    for (std::size_t trail_idx = 0; trail_idx < 4; ++trail_idx) {
        trail.store_reason_coordinates(trail_idx, TrailCoordinates{});
    }

    // A level-1 learn goes in at trail index 2, so the rows from there on moved.
    trail.learn(make_equation_with(repr, 6, {4}), ClauseId{2}, 1);
    REQUIRE(trail.span().rows() == 5);
    CHECK(trail.reason_coordinates(0) != nullptr);
    CHECK(trail.reason_coordinates(1) != nullptr);
    for (std::size_t trail_idx = 2; trail_idx < 5; ++trail_idx) {
        CHECK(trail.reason_coordinates(trail_idx) == nullptr);
    }

    // A backjump keeps what it keeps of them.
    trail.store_reason_coordinates(3, TrailCoordinates{});
    trail.resize(2);
    REQUIRE(trail.span().rows() == 3);
    CHECK(trail.reason_coordinates(1) != nullptr);
    CHECK(trail.reason_coordinates(2) == nullptr);
}

TEST_CASE_TEMPLATE("trail: an out-of-order learn on a free column leaves every row alone", Tag,
                   DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    Trail trail(repr, 7);

    trail.learn(make_equation_with(repr, 7, {0}), std::nullopt, 1);
    trail.learn(make_equation_with(repr, 7, {1}), std::nullopt, 2);
    trail.learn(make_equation_with(repr, 7, {3}), ClauseId{0}, 1);

    const std::vector<std::vector<std::size_t>> expected{{0}, {3}, {1}};
    CHECK(rows_in_trail_order(trail) == expected);
    CHECK(trail.trail_idx_to_row_idx() == std::vector<std::size_t>{0, 2, 1});
    CHECK(trail.rows_at_level(1) == 2);
    CHECK(trail.level_of(1) == 1);
}

TEST_CASE_TEMPLATE("trail: an out-of-order learn takes the pivot of a later row", Tag, DenseTag,
                   SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    Trail trail(repr, 7);

    // Level 1 holds `x1 = 0` and level 2 decides `x2 = 0`; learning
    // `x1 + x2 + x3 = 0` at level 1 stores `x2 + x3 = 0` there and leaves the
    // decision holding `x3 = 0`.
    trail.learn(make_equation_with(repr, 7, {0}), std::nullopt, 1);
    trail.learn(make_equation_with(repr, 7, {1}), std::nullopt, 2);
    trail.learn(make_equation_with(repr, 7, {0, 1, 2}), ClauseId{0}, 1);

    const std::vector<std::vector<std::size_t>> expected{{0}, {1, 2}, {2}};
    CHECK(rows_in_trail_order(trail) == expected);
    CHECK(trail.level_of(1) == 1);
    CHECK(trail.level_of(2) == 2);
    CHECK(trail.reasons()[1] == std::optional<ClauseId>{0});
}

TEST_CASE_TEMPLATE("trail: an out-of-order learn displaces a row twice over", Tag, DenseTag,
                   SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    Trail trail(repr, 7);

    trail.learn(make_equation_with(repr, 7, {0}), std::nullopt, 1);
    trail.learn(make_equation_with(repr, 7, {1, 2}), std::nullopt, 2);
    trail.learn(make_equation_with(repr, 7, {2, 4}), ClauseId{1}, 2);

    trail.learn(make_equation_with(repr, 7, {0, 1, 3}), ClauseId{2}, 1);

    // Column 1 goes to the new row, column 2 to the row that held column 1, and
    // the row that held column 2 ends up on the free column 3.
    const std::vector<std::vector<std::size_t>> expected{{0}, {1, 3}, {2, 3}, {3, 4}};
    CHECK(rows_in_trail_order(trail) == expected);
    CHECK(trail.rows_at_level(1) == 2);
    CHECK(trail.level_of(1) == 1);
    CHECK(trail.level_of(2) == 2);
}

TEST_CASE_TEMPLATE("trail: a displaced row reduces by an earlier row before colliding again", Tag,
                   DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    Trail trail(repr, 7);

    trail.learn(make_equation_with(repr, 7, {0}), std::nullopt, 1);
    trail.learn(make_equation_with(repr, 7, {2}), std::nullopt, 2);
    trail.learn(make_equation_with(repr, 7, {1, 2}), ClauseId{1}, 2);
    trail.learn(make_equation_with(repr, 7, {3, 5}), ClauseId{2}, 2);

    trail.learn(make_equation_with(repr, 7, {0, 1, 3}), ClauseId{3}, 1);

    // The row displaced from column 1 becomes `x3 + x4 = 0`, which the earlier
    // decision on column 2 reduces to `x4 = 0` before it takes column 3.
    const std::vector<std::vector<std::size_t>> expected{{0}, {1, 3}, {2}, {3}, {5}};
    CHECK(rows_in_trail_order(trail) == expected);
    CHECK(trail.trail_idx_to_row_idx() == std::vector<std::size_t>{0, 1, 2, 3, 4});
    CHECK(trail.rows_at_level(1) == 2);
}

TEST_CASE_TEMPLATE("trail: learning matches reinsertion on random trails", Tag, DenseTag,
                   SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    constexpr std::size_t num_vars = 12;
    TestRandom random(20260920);

    std::size_t out_of_order_learns = 0;
    for (int trial = 0; trial < 300; ++trial) {
        Trail trail(repr, num_vars);
        ReferenceTrail reference;

        for (int step = 0; step < 16; ++step) {
            const Equation candidate = random_equation(random, repr, num_vars);
            Equation reduced = candidate;
            trail.span().reduce(reduced);
            if (reduced.is_eq_true()) {
                continue; // `learn` takes only an equation the span does not hold
            }

            const bool decide = trail.span().rows() == 0 || random.chance(3);
            const std::size_t level = decide ? trail.level() + 1 : random.next(trail.level() + 1);
            const std::size_t trail_idx =
                decide ? trail.span().rows() : trail.rows_at_level(level);
            const std::optional<ClauseId> reason =
                decide ? std::nullopt : std::optional<ClauseId>{ClauseId{1}};

            REQUIRE(trail.learn(candidate, reason, level) == trail_idx);
            reference.learn(candidate, trail_idx);
            out_of_order_learns += trail_idx + 1 < trail.span().rows() ? 1U : 0U;
            REQUIRE(rows_in_trail_order(trail) == reference.rows_in_trail_order());
        }
    }
    CHECK(out_of_order_learns > 1000);
}

} // namespace xorcle
