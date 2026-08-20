#include <doctest/doctest.h>

#include <cstddef>
#include <vector>

#include "algebra/compaction_transform.h"
#include "algebra/equation.h"
#include "algebra/row_echelon_matrix.h"
#include "algebra_test_support.h"
#include "heuristics/decisions/clause_decisions.h"
#include "heuristics_test_support.h"
#include "util/index_set.h"
#include "util/random.h"

namespace xorcle {

TEST_CASE_TEMPLATE("clause decisions: clause satisfaction", Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    ClauseSatisfactionWorkspace workspace(repr, 4);

    SUBCASE("a clause whose row is falsified by the trail is satisfied") {
        // The clause is [x0 = 1], stored as the row [x0 = 0]. With x0 = 1 on the
        // trail that row reduces to the false equation, so the clause holds.
        const RowSpace span = make_row_space(repr, 4, {{0, 4}});
        const Clause clause = make_clause(repr, 4, {{0}});
        CHECK(clause_satisfied_by_trail(span, clause, workspace));
    }

    SUBCASE("a clause whose row is implied by the trail is not satisfied") {
        const RowSpace span = make_row_space(repr, 4, {{0}});
        const Clause clause = make_clause(repr, 4, {{0}});
        CHECK_FALSE(clause_satisfied_by_trail(span, clause, workspace));
    }

    SUBCASE("satisfaction can need a combination of rows") {
        // [x0 = 1] v [x1 = 1] against x0 + x1 = 1: neither row alone reduces to
        // the false equation, but their sum does.
        const RowSpace span = make_row_space(repr, 4, {{0, 1, 4}});
        const Clause clause = make_clause(repr, 4, {{0}, {1}});
        REQUIRE(clause.rows() == 2);
        CHECK(clause_satisfied_by_trail(span, clause, workspace));
    }

    SUBCASE("an unrelated trail leaves the clause open") {
        const RowSpace span = make_row_space(repr, 4, {{3, 4}});
        const Clause clause = make_clause(repr, 4, {{0}, {1}});
        CHECK_FALSE(clause_satisfied_by_trail(span, clause, workspace));
    }

    SUBCASE("everything is satisfied once the trail spans the false equation") {
        const RowSpace span = make_row_space(repr, 4, {{4}});
        const Clause clause = make_clause(repr, 4, {{0}, {1}});
        CHECK(clause_satisfied_by_trail(span, clause, workspace));
    }

    SUBCASE("the workspace is reusable across calls and variable counts") {
        const RowSpace wide_span = make_row_space(repr, 4, {{0, 4}});
        const Clause wide_clause = make_clause(repr, 4, {{0}, {1}});
        CHECK(clause_satisfied_by_trail(wide_span, wide_clause, workspace));
        CHECK(clause_satisfied_by_trail(wide_span, wide_clause, workspace));

        workspace.reconfigure(2);
        const RowSpace narrow_span = make_row_space(repr, 2, {{0}});
        const Clause narrow_clause = make_clause(repr, 2, {{0}});
        CHECK_FALSE(clause_satisfied_by_trail(narrow_span, narrow_clause, workspace));
    }
}

TEST_CASE_TEMPLATE("clause decisions: decide from a basis", Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    std::vector<Equation> basis;
    for (std::size_t var = 0; var < 4; ++var) {
        basis.push_back(make_equation_with(repr, 4, {var}));
    }
    Equation scratch(repr, 4);

    SUBCASE("an empty trail decides the first basis row") {
        const RowSpace span(repr, 4);
        CHECK(decide_from_basis(basis, span, scratch) == basis[0]);
    }

    SUBCASE("determined variables are skipped") {
        const RowSpace span = make_row_space(repr, 4, {{0, 4}, {1}});
        CHECK(decide_from_basis(basis, span, scratch) == basis[2]);
    }

    SUBCASE("the returned row is the unreduced basis row") {
        // x0 is determined, and x1 is legal only after reducing to x2. The
        // caller gets x1 back, not its reduction, and re-reduces it itself.
        const RowSpace span = make_row_space(repr, 4, {{0}, {1, 2}});
        const Equation& decision = decide_from_basis(basis, span, scratch);
        CHECK(bits_of(decision) == std::vector<std::size_t>{1});
        CHECK(bits_of(scratch) == std::vector<std::size_t>{2});
    }
}

TEST_CASE_TEMPLATE("clause decisions: compact a basis", Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;

    // Eliminate x1 out of four variables, so the survivors are x0, x2, x3.
    IndexSet dropped(5);
    dropped.set(1);
    const CompactionTransform transform = CompactionTransform::from_dropped_columns(4, dropped);
    REQUIRE(transform.new_num_vars() == 3);

    SUBCASE("a literal basis stays one equation per new leading bit") {
        std::vector<Equation> basis;
        for (std::size_t var = 0; var < 4; ++var) {
            basis.push_back(make_equation_with(repr, 4, {var}));
        }

        const std::vector<Equation> compacted = compact_basis_keep_order(basis, transform, repr);
        REQUIRE(compacted.size() == 3);
        CHECK(bits_of(compacted[0]) == std::vector<std::size_t>{0});
        CHECK(bits_of(compacted[1]) == std::vector<std::size_t>{1});
        CHECK(bits_of(compacted[2]) == std::vector<std::size_t>{2});
    }

    SUBCASE("collapsed and duplicated rows are replaced by fresh literals") {
        // x1 compacts to the true equation, and x1 + x2 collides with x2.
        std::vector<Equation> basis{
            make_equation_with(repr, 4, {1}),
            make_equation_with(repr, 4, {1, 2}),
            make_equation_with(repr, 4, {2}),
            make_equation_with(repr, 4, {4}),
        };

        const std::vector<Equation> compacted = compact_basis_keep_order(basis, transform, repr);
        REQUIRE(compacted.size() == 3);
        // x1 + x2 survives as x2's compacted literal, in old basis order.
        CHECK(bits_of(compacted[0]) == std::vector<std::size_t>{1});
        // Then the uncovered new variables, ascending.
        CHECK(bits_of(compacted[1]) == std::vector<std::size_t>{0});
        CHECK(bits_of(compacted[2]) == std::vector<std::size_t>{2});
    }
}

TEST_CASE_TEMPLATE("clause decisions: sample an equation from a clause", Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    const RowSpace span(repr, 4);
    Equation candidate(repr, 4);
    Equation reduced(repr, 4);
    Rng rng = make_rng(7, RngComponent::csids_equation);

    SUBCASE("a unit clause always samples its only row") {
        const Clause clause = make_clause(repr, 4, {{1, 4}});
        for (int attempt = 0; attempt < 8; ++attempt) {
            REQUIRE(sample_equation_from_clause(clause, span, rng, candidate, reduced));
            CHECK(bits_of(candidate) == std::vector<std::size_t>{1, 4});
            CHECK(candidate == reduced);
        }
    }

    SUBCASE("every sample is a non-zero combination of the rows") {
        const Clause clause = make_clause(repr, 4, {{0}, {1}});
        bool saw_sum = false;
        for (int attempt = 0; attempt < 64; ++attempt) {
            REQUIRE(sample_equation_from_clause(clause, span, rng, candidate, reduced));
            const std::vector<std::size_t> bits = bits_of(candidate);
            const bool is_row0 = bits == std::vector<std::size_t>{0};
            const bool is_row1 = bits == std::vector<std::size_t>{1};
            const bool is_sum = bits == std::vector<std::size_t>{0, 1};
            CHECK((is_row0 || is_row1 || is_sum));
            saw_sum = saw_sum || is_sum;
        }
        CHECK(saw_sum);
    }

    SUBCASE("rows already implied by the trail are rejected") {
        // x0 is on the trail, so the row [x0 = 0] reduces away and only
        // combinations involving the second row can be returned.
        const RowSpace determined = make_row_space(repr, 4, {{0}});
        const Clause clause = make_clause(repr, 4, {{0}, {1}});
        for (int attempt = 0; attempt < 32; ++attempt) {
            REQUIRE(sample_equation_from_clause(clause, determined, rng, candidate, reduced));
            CHECK(bits_of(reduced) == std::vector<std::size_t>{1});
        }
    }

    SUBCASE("the draw is a deterministic function of the seed") {
        const Clause clause = make_clause(repr, 4, {{0}, {1}, {2}});
        Rng first = make_rng(11, RngComponent::csids_equation);
        Rng second = make_rng(11, RngComponent::csids_equation);
        Equation other_candidate(repr, 4);
        Equation other_reduced(repr, 4);
        for (int attempt = 0; attempt < 16; ++attempt) {
            REQUIRE(sample_equation_from_clause(clause, span, first, candidate, reduced));
            REQUIRE(sample_equation_from_clause(clause, span, second, other_candidate,
                                                other_reduced));
            CHECK(candidate == other_candidate);
        }
    }
}

} // namespace xorcle
