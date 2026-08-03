#include <doctest/doctest.h>

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

/// @brief The trail-order sequence of set columns, resolved through `order_map()`.
std::vector<std::vector<std::size_t>> rows_in_trail_order(const TrailSnapshot& snapshot) {
    std::vector<std::vector<std::size_t>> out;
    for (const std::size_t row_index : snapshot.order_map()) {
        out.push_back(columns_of(snapshot.span().row(row_index)));
    }
    return out;
}

} // namespace

TEST_CASE_TEMPLATE("trail: root snapshot", Tag, DenseTag, SparseTag) {
    constexpr Algebra backend = Tag::value;
    const Trail trail(backend, 4);

    CHECK(trail.size() == 1);
    CHECK(trail.level() == 0);
    CHECK(trail.backend() == backend);
    CHECK(trail.num_vars() == 4);
    CHECK(trail.span().rows() == 0);
    CHECK(trail.span().cols() == 5);
    CHECK(trail.order_map().empty());
    CHECK(trail.reverse_order_map().empty());
    CHECK(trail.reasons().empty());
}

TEST_CASE_TEMPLATE("trail: propagations stay on the current level", Tag, DenseTag, SparseTag) {
    constexpr Algebra backend = Tag::value;
    Trail trail(backend, 4);

    trail.learn(make_equation_with(backend, 4, {0, 4}), ClauseId{7});
    CHECK(trail.size() == 1);
    CHECK(trail.level() == 0);
    CHECK(trail.span().rows() == 1);
    REQUIRE(trail.reasons().size() == 1);
    CHECK(trail.reasons()[0] == std::optional<ClauseId>{7});

    trail.learn(make_equation_with(backend, 4, {1}), ClauseId{2});
    CHECK(trail.size() == 1);
    CHECK(trail.span().rows() == 2);
    CHECK(trail.reasons()[1] == std::optional<ClauseId>{2});
}

TEST_CASE_TEMPLATE("trail: decisions push an independent snapshot", Tag, DenseTag, SparseTag) {
    constexpr Algebra backend = Tag::value;
    Trail trail(backend, 4);

    trail.learn(make_equation_with(backend, 4, {0, 4}), ClauseId{7});
    trail.learn(make_equation_with(backend, 4, {1}), std::nullopt);

    CHECK(trail.size() == 2);
    CHECK(trail.level() == 1);
    CHECK(trail.span().rows() == 2);
    CHECK(trail.reasons()[1] == std::nullopt);

    // The earlier level keeps its own matrix. Computing the asserting level
    // reads `trail[k].span().rows()` per level, so this must not be shared.
    CHECK(trail[0].span().rows() == 1);
    CHECK(trail[1].span().rows() == 2);
    CHECK(trail[0].reasons().size() == 1);

    // A non-root snapshot begins, in trail order, with that level's decision.
    CHECK(rows_in_trail_order(trail[1])[1] == std::vector<std::size_t>{1});

    trail.learn(make_equation_with(backend, 4, {2}), ClauseId{5});
    CHECK(trail.size() == 2);
    CHECK(trail[0].span().rows() == 1);
    CHECK(trail[1].span().rows() == 3);
}

TEST_CASE_TEMPLATE("trail: order maps are inverse permutations", Tag, DenseTag, SparseTag) {
    constexpr Algebra backend = Tag::value;
    Trail trail(backend, 4);

    // Learned leading columns 2, 0, 3 in that order, so row-echelon order and
    // trail order genuinely disagree and the insertion shift is exercised.
    trail.learn(make_equation_with(backend, 4, {2}), ClauseId{0});
    CHECK(trail.order_map() == std::vector<std::size_t>{0});
    CHECK(trail.reverse_order_map() == std::vector<std::size_t>{0});

    trail.learn(make_equation_with(backend, 4, {0, 4}), ClauseId{1});
    CHECK(trail.order_map() == std::vector<std::size_t>{1, 0});
    CHECK(trail.reverse_order_map() == std::vector<std::size_t>{1, 0});

    trail.learn(make_equation_with(backend, 4, {3, 4}), ClauseId{2});
    CHECK(trail.order_map() == std::vector<std::size_t>{1, 0, 2});
    CHECK(trail.reverse_order_map() == std::vector<std::size_t>{1, 0, 2});

    // Every trail step resolves to the row it created, up to reduction by the
    // rows already present.
    const std::vector<std::vector<std::size_t>> expected{{2}, {0, 4}, {3, 4}};
    CHECK(rows_in_trail_order(trail.back()) == expected);

    // The maps really are inverse permutations of each other.
    for (std::size_t t = 0; t < trail.order_map().size(); ++t) {
        CHECK(trail.reverse_order_map()[trail.order_map()[t]] == t);
    }
    for (std::size_t r = 0; r < trail.reverse_order_map().size(); ++r) {
        CHECK(trail.order_map()[trail.reverse_order_map()[r]] == r);
    }
    CHECK(trail.reasons().size() == trail.span().rows());
}

TEST_CASE_TEMPLATE("trail: learning a partially reduced equation", Tag, DenseTag, SparseTag) {
    constexpr Algebra backend = Tag::value;
    Trail trail(backend, 4);

    trail.learn(make_equation_with(backend, 4, {0, 1}), ClauseId{0});

    // The solver always partially reduces before learning, which is what the
    // precondition on `learn()` asks for.
    Equation candidate = make_equation_with(backend, 4, {0, 2});
    const ReduceResult reduced = trail.span().partial_reduce(candidate);
    CHECK(reduced == ReduceResult::leading(1));
    CHECK(columns_of(candidate) == std::vector<std::size_t>{1, 2});
    trail.learn(candidate, ClauseId{1});

    CHECK(trail.span().rows() == 2);
    CHECK(trail.order_map() == std::vector<std::size_t>{0, 1});
    const std::vector<std::vector<std::size_t>> expected{{0, 1}, {1, 2}};
    CHECK(rows_in_trail_order(trail.back()) == expected);
}

TEST_CASE_TEMPLATE("trail: resize truncates trailing levels", Tag, DenseTag, SparseTag) {
    constexpr Algebra backend = Tag::value;
    Trail trail(backend, 4);

    trail.learn(make_equation_with(backend, 4, {0, 4}), ClauseId{9});
    trail.learn(make_equation_with(backend, 4, {1}), std::nullopt);
    trail.learn(make_equation_with(backend, 4, {2}), ClauseId{3});
    trail.learn(make_equation_with(backend, 4, {3}), std::nullopt);
    REQUIRE(trail.size() == 3);
    REQUIRE(trail.span().rows() == 4);

    SUBCASE("backjump to an intermediate level") {
        const std::size_t asserting_level = 1;
        trail.resize(asserting_level + 1);
        CHECK(trail.size() == 2);
        CHECK(trail.level() == 1);
        CHECK(trail.span().rows() == 3);
        CHECK(trail.reasons().size() == 3);
    }

    SUBCASE("restart back to the root") {
        trail.resize(1);
        CHECK(trail.size() == 1);
        CHECK(trail.level() == 0);
        CHECK(trail.span().rows() == 1);
        const std::vector<std::optional<ClauseId>> expected_reasons{std::optional<ClauseId>{9}};
        CHECK(trail.reasons() == expected_reasons);
        CHECK(trail.order_map() == std::vector<std::size_t>{0});
    }

    SUBCASE("truncating to the current size is a no-op") {
        trail.resize(trail.size());
        CHECK(trail.size() == 3);
        CHECK(trail.span().rows() == 4);
    }
}

TEST_CASE_TEMPLATE("trail: learning after a backjump reuses the restored level", Tag, DenseTag,
                   SparseTag) {
    constexpr Algebra backend = Tag::value;
    Trail trail(backend, 4);

    trail.learn(make_equation_with(backend, 4, {0}), std::nullopt);
    trail.learn(make_equation_with(backend, 4, {1}), std::nullopt);
    REQUIRE(trail.size() == 3);

    trail.resize(1);
    trail.learn(make_equation_with(backend, 4, {2, 4}), ClauseId{4});

    CHECK(trail.size() == 1);
    CHECK(trail.span().rows() == 1);
    CHECK(columns_of(trail.span().row(0)) == std::vector<std::size_t>{2, 4});
    const std::vector<std::optional<ClauseId>> expected_reasons{std::optional<ClauseId>{4}};
    CHECK(trail.reasons() == expected_reasons);
}

} // namespace xorcle
