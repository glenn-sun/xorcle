#include <cstddef>
#include <initializer_list>
#include <vector>

#include <doctest/doctest.h>

#include "algebra/row_echelon_matrix.h"
#include "algebra_test_support.h"
#include "core/clause_id.h"
#include "heuristics/equivalent_clauses/equivalent_clause_table.h"
#include "heuristics_test_support.h"

namespace xorcle {

namespace {

Clause reduced_clause(EquationRepr repr, std::size_t num_vars,
                      std::initializer_list<std::initializer_list<std::size_t>> rows) {
    Clause clause = make_clause(repr, num_vars, rows);
    clause.reduce_rows();
    return clause;
}

} // namespace

TEST_CASE_TEMPLATE("equivalent clause table: one span is found whatever basis it was given in",
                   Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    std::vector<std::size_t> var_to_name = {0, 1, 2, 3};
    const auto key = [&var_to_name](const Clause& clause) { return clause.rows_hash(var_to_name); };
    std::vector<Clause> clauses;
    clauses.push_back(reduced_clause(repr, 4, {{1, 2, 4}, {2, 3}}));
    const Clause same_span = reduced_clause(repr, 4, {{1, 3, 4}, {2, 3}});
    const Clause other_constant = reduced_clause(repr, 4, {{1, 3}, {2, 3}});
    const Clause subspace = reduced_clause(repr, 4, {{1, 2, 4}});

    CHECK(clauses[0].has_rows_of(same_span));
    CHECK(key(clauses[0]) == key(same_span));
    CHECK(!clauses[0].has_rows_of(other_constant));
    CHECK(!clauses[0].has_rows_of(subspace));

    EquivalentClauseTable table;
    CHECK(!table.find(same_span, key(same_span), clauses).has_value());
    table.enter(0, key(clauses[0]));
    CHECK(table.is_entered(0));
    CHECK(table.find(same_span, key(same_span), clauses) == ClauseId{0});
    CHECK(!table.find(other_constant, key(other_constant), clauses).has_value());
    CHECK(!table.find(subspace, key(subspace), clauses).has_value());

    SUBCASE("a removed clause is not found, and its id can be entered again") {
        table.remove(0);
        CHECK(table.size() == 0);
        CHECK(!table.find(same_span, key(same_span), clauses).has_value());
        table.enter(0, key(clauses[0]));
        CHECK(table.find(same_span, key(same_span), clauses) == ClauseId{0});
    }

    SUBCASE("find_or_enter finds the entered clause, and enters one it does not find") {
        clauses.push_back(other_constant);
        CHECK(table.find_or_enter(same_span, key(same_span), 1, clauses) == ClauseId{0});
        CHECK(!table.is_entered(1));
        CHECK(!table.find_or_enter(other_constant, key(other_constant), 1, clauses).has_value());
        CHECK(table.is_entered(1));
        CHECK(table.size() == 2);
        CHECK(table.find(other_constant, key(other_constant), clauses) == ClauseId{1});
    }

    SUBCASE("a renumbering keeps the entries") {
        // x0 is dropped: the same clause over x1..x3 renumbered 0..2.
        clauses[0] = reduced_clause(repr, 3, {{0, 1, 3}, {1, 2}});
        var_to_name = {1, 2, 3};
        const Clause renumbered = reduced_clause(repr, 3, {{0, 2, 3}, {0, 1, 3}});
        CHECK(table.find(renumbered, key(renumbered), clauses) == ClauseId{0});
    }
}

TEST_CASE("equivalent clause table: entries survive growth and removals") {
    constexpr EquationRepr repr = EquationRepr::dense;
    constexpr std::size_t num_vars = 12;
    std::vector<std::size_t> var_to_name(num_vars);
    for (std::size_t var = 0; var < num_vars; ++var) {
        var_to_name[var] = 3 * var;
    }
    const auto key = [&var_to_name](const Clause& clause) { return clause.rows_hash(var_to_name); };
    // Every two-variable equation with either constant: 132 distinct one-row clauses.
    std::vector<Clause> clauses;
    for (std::size_t first = 0; first < num_vars; ++first) {
        for (std::size_t second = first + 1; second < num_vars; ++second) {
            clauses.push_back(reduced_clause(repr, num_vars, {{first, second}}));
            clauses.push_back(reduced_clause(repr, num_vars, {{first, second, num_vars}}));
        }
    }
    EquivalentClauseTable table;
    for (ClauseId id = 0; id < clauses.size(); ++id) {
        CHECK(!table.find(clauses[id], key(clauses[id]), clauses).has_value());
        table.enter(id, key(clauses[id]));
    }
    CHECK(table.size() == clauses.size());
    for (ClauseId id = 0; id < clauses.size(); id += 2) {
        table.remove(id);
    }
    // Removed ids are entered and removed again.
    for (int round = 0; round < 4; ++round) {
        for (ClauseId id = 0; id < clauses.size(); id += 2) {
            table.enter(id, key(clauses[id]));
        }
        for (ClauseId id = 0; id < clauses.size(); ++id) {
            CHECK(table.find(clauses[id], key(clauses[id]), clauses) == id);
        }
        for (ClauseId id = 0; id < clauses.size(); id += 2) {
            table.remove(id);
        }
    }
    for (ClauseId id = 0; id < clauses.size(); ++id) {
        const bool entered = id % 2 == 1;
        CHECK(table.is_entered(id) == entered);
        CHECK(table.find(clauses[id], key(clauses[id]), clauses).has_value() == entered);
    }
}

} // namespace xorcle
