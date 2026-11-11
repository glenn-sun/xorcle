#include <doctest/doctest.h>

#include "algebra/row_echelon_matrix.h"
#include "algebra_test_support.h"
#include "heuristics/decisions/clause_decisions.h"
#include "heuristics_test_support.h"

namespace xorcle {

TEST_CASE_TEMPLATE("clause decisions: clause status", Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    ClauseSatisfactionWorkspace workspace(repr, 4);

    SUBCASE("a clause whose row is falsified by the trail is satisfied") {
        // The clause is [x0 = 1], stored as the row [x0 = 0]. With x0 = 1 on the
        // trail that row reduces to the false equation, so the clause holds.
        const RowSpace span = make_row_space(repr, 4, {{0, 4}});
        const Clause clause = make_clause(repr, 4, {{0}});
        CHECK(clause_status_by_trail(span, clause, workspace) == ClauseStatus::satisfied);
    }

    SUBCASE("a clause whose every row is implied by the trail propagates") {
        const RowSpace span = make_row_space(repr, 4, {{0}});
        const Clause clause = make_clause(repr, 4, {{0}});
        CHECK(clause_status_by_trail(span, clause, workspace) == ClauseStatus::propagating);
    }

    SUBCASE("a clause of dimension one propagates rather than offering a decision") {
        const RowSpace empty(repr, 4);
        const Clause clause = make_clause(repr, 4, {{0}});
        CHECK(clause_status_by_trail(empty, clause, workspace) == ClauseStatus::propagating);
    }

    SUBCASE("a wide clause stays open until the trail takes its dimension to one") {
        const Clause clause = make_clause(repr, 4, {{0}, {1}, {2}});
        const RowSpace one_row = make_row_space(repr, 4, {{0}});
        CHECK(clause_status_by_trail(one_row, clause, workspace) == ClauseStatus::open);
        const RowSpace two_rows = make_row_space(repr, 4, {{0}, {1}});
        CHECK(clause_status_by_trail(two_rows, clause, workspace) == ClauseStatus::propagating);
        const RowSpace every_row = make_row_space(repr, 4, {{0}, {1}, {2}});
        CHECK(clause_status_by_trail(every_row, clause, workspace) == ClauseStatus::propagating);
    }

    SUBCASE("satisfaction can need a combination of rows") {
        // [x0 = 1] v [x1 = 1] against x0 + x1 = 1: neither row alone reduces to
        // the false equation, but their sum does.
        const RowSpace span = make_row_space(repr, 4, {{0, 1, 4}});
        const Clause clause = make_clause(repr, 4, {{0}, {1}});
        REQUIRE(clause.rows() == 2);
        CHECK(clause_status_by_trail(span, clause, workspace) == ClauseStatus::satisfied);
    }

    SUBCASE("an unrelated trail leaves the clause open") {
        const RowSpace span = make_row_space(repr, 4, {{3, 4}});
        const Clause clause = make_clause(repr, 4, {{0}, {1}});
        CHECK(clause_status_by_trail(span, clause, workspace) == ClauseStatus::open);
    }

    SUBCASE("everything is satisfied once the trail spans the false equation") {
        const RowSpace span = make_row_space(repr, 4, {{4}});
        const Clause clause = make_clause(repr, 4, {{0}, {1}});
        CHECK(clause_status_by_trail(span, clause, workspace) == ClauseStatus::satisfied);
    }

    SUBCASE("the workspace is reusable across calls and variable counts") {
        const RowSpace wide_span = make_row_space(repr, 4, {{0, 4}});
        const Clause wide_clause = make_clause(repr, 4, {{0}, {1}});
        CHECK(clause_status_by_trail(wide_span, wide_clause, workspace) ==
              ClauseStatus::satisfied);
        CHECK(clause_status_by_trail(wide_span, wide_clause, workspace) ==
              ClauseStatus::satisfied);

        workspace.reconfigure(2);
        const RowSpace narrow_span = make_row_space(repr, 2, {{0}});
        const Clause narrow_clause = make_clause(repr, 2, {{0}});
        CHECK(clause_status_by_trail(narrow_span, narrow_clause, workspace) ==
              ClauseStatus::propagating);
    }
}

} // namespace xorcle
