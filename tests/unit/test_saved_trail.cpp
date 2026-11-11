#include <doctest/doctest.h>

#include <cstddef>
#include <optional>
#include <vector>

#include "algebra/compaction_transform.h"
#include "algebra/equation_repr.h"
#include "algebra/row_echelon_matrix.h"
#include "algebra_test_support.h"
#include "core/clause_id.h"
#include "core/saved_trail.h"
#include "core/trail.h"
#include "util/index_set.h"

namespace xorcle {

namespace {

constexpr std::size_t num_vars = 6;

/// @brief Root `x0 = 0`; level 1 decides `x1 + x2 = 0` and propagates `x0 + x3 = 0`;
/// level 2 decides `x4 = 0`.
Trail make_trail(EquationRepr repr) {
    Trail trail(repr, num_vars);
    trail.learn(make_equation_with(repr, num_vars, {0}), ClauseId{0}, 0);
    trail.learn(make_equation_with(repr, num_vars, {1, 2}), std::nullopt, 1);
    trail.learn(make_equation_with(repr, num_vars, {0, 3}), ClauseId{0}, 1);
    trail.learn(make_equation_with(repr, num_vars, {4}), std::nullopt, 2);
    return trail;
}

std::vector<std::vector<std::size_t>> saved_rows(const SavedTrail& saved) {
    std::vector<std::vector<std::size_t>> rows;
    for (std::size_t row_idx = 0; row_idx < saved.num_rows(); ++row_idx) {
        rows.push_back(bits_of(saved.rows[row_idx]));
    }
    return rows;
}

} // namespace

TEST_CASE_TEMPLATE("saved trail: a substitution and a renumbering rewrite it as the clauses",
                   Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    const Trail trail = make_trail(repr);
    const std::vector<std::size_t> stamps{1, 5, 7};

    SavedTrail saved;
    saved.save(trail, stamps, 2, trail.span().rows());
    REQUIRE(saved.num_rows() == 3);
    REQUIRE(saved.level_ends == std::vector<std::size_t>{2, 3});
    REQUIRE(saved.root_rows == 1);

    // The root as it stands at the substitution: `x0 = 0` and, found since, `x1 = 1`.
    RowSpace root_span(repr, num_vars);
    root_span.add_row(make_equation_with(repr, num_vars, {0}));
    root_span.add_row(make_equation_with(repr, num_vars, {1, num_vars}));
    saved.substitute(root_span);

    CHECK(saved_rows(saved) ==
          std::vector<std::vector<std::size_t>>{{2, num_vars}, {3}, {4}});
    CHECK(saved.level_ends == std::vector<std::size_t>{2, 3});
    CHECK(saved.level_stamps == std::vector<std::size_t>(3, unshared_level_stamp));
    CHECK(saved.root_rows == 0);

    IndexSet dropped(num_vars + 1);
    dropped.set(0);
    dropped.set(1);
    saved.renumber(CompactionTransform::from_dropped_columns(num_vars, dropped));
    CHECK(saved_rows(saved) == std::vector<std::vector<std::size_t>>{{0, 4}, {1}, {2}});
    CHECK(saved.rows[0].num_bits() == num_vars - 1);
}

TEST_CASE_TEMPLATE("saved trail: a row the root implies or contradicts stays in place", Tag,
                   DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    const Trail trail = make_trail(repr);
    SavedTrail saved;
    saved.save(trail, std::vector<std::size_t>{1, 5, 7}, 2, trail.span().rows());

    // `x4 = 1` contradicts level 2's row, and `x3 = 0` implies level 1's second.
    RowSpace root_span(repr, num_vars);
    root_span.add_row(make_equation_with(repr, num_vars, {0}));
    root_span.add_row(make_equation_with(repr, num_vars, {3}));
    root_span.add_row(make_equation_with(repr, num_vars, {4, num_vars}));
    saved.substitute(root_span);

    CHECK(saved_rows(saved) ==
          std::vector<std::vector<std::size_t>>{{1, 2}, {}, {num_vars}});
    CHECK(saved.level_ends == std::vector<std::size_t>{2, 3});
}

} // namespace xorcle
