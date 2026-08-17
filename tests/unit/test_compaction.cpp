#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <vector>

#include "algebra/compaction_transform.h"
#include "algebra/equation.h"
#include "algebra/row_echelon_matrix.h"
#include "algebra_test_support.h"
#include "core/clause_id.h"
#include "core/trail.h"
#include "heuristics/compaction/compaction.h"
#include "heuristics/compaction/original_space.h"
#include "heuristics_test_support.h"
#include "util/bitmap.h"

namespace xorcle {

namespace {

std::vector<std::size_t> set_variables(const Bitmap& assignment) {
    std::vector<std::size_t> out;
    assignment.for_each_set([&](std::size_t index) { out.push_back(index); });
    return out;
}

} // namespace

TEST_CASE_TEMPLATE("compaction: when it should run", Tag, DenseTag, SparseTag) {
    constexpr Algebra backend = Tag::value;
    Trail trail(backend, 4);
    const std::vector<Clause> clauses;
    const std::vector<std::uint8_t> active;
    const std::vector<std::size_t> var_map{0, 1, 2, 3};

    const auto inputs = [&](bool has_compacted_once, std::size_t num_conflicts,
                            std::size_t last_conflicts) {
        return CompactionInputs{trail, clauses, active, var_map, 4, has_compacted_once,
                                num_conflicts, last_conflicts, 16};
    };

    SUBCASE("an empty root trail has nothing to eliminate") {
        CHECK_FALSE(should_compact_before_decision(inputs(false, 0, 0)));
    }

    SUBCASE("a pivoted root trail is compactable") {
        trail.learn(make_equation_with(backend, 4, {1, 4}), ClauseId{0});
        CHECK(should_compact_before_decision(inputs(false, 0, 0)));
    }

    SUBCASE("only the root level is compactable") {
        trail.learn(make_equation_with(backend, 4, {1, 4}), ClauseId{0});
        trail.learn(make_equation_with(backend, 4, {2}), std::nullopt);
        REQUIRE(trail.size() == 2);
        CHECK_FALSE(should_compact_before_decision(inputs(false, 0, 0)));
    }

    SUBCASE("the conflict interval gates repeat compactions") {
        trail.learn(make_equation_with(backend, 4, {1, 4}), ClauseId{0});
        CHECK_FALSE(should_compact_before_decision(inputs(true, 10, 0)));
        CHECK(should_compact_before_decision(inputs(true, 16, 0)));
    }
}

TEST_CASE_TEMPLATE("compaction: building the transform", Tag, DenseTag, SparseTag) {
    constexpr Algebra backend = Tag::value;
    Trail trail(backend, 4);
    trail.learn(make_equation_with(backend, 4, {0, 4}), ClauseId{0});
    trail.learn(make_equation_with(backend, 4, {2, 3}), ClauseId{1});

    const CompactionTransform transform = build_compaction_transform(trail, 4);
    CHECK(transform.old_num_vars() == 4);
    CHECK(transform.new_num_vars() == 2);
    CHECK(transform.to_old(0) == 1);
    CHECK(transform.to_old(1) == 3);
    CHECK(transform.to_new(1) == 0);
    CHECK(transform.to_new(3) == 1);
    CHECK(transform.to_new(0) == CompactionTransform::npos);
}

TEST_CASE_TEMPLATE("compaction: rewriting the problem", Tag, DenseTag, SparseTag) {
    constexpr Algebra backend = Tag::value;
    Trail trail(backend, 4);
    trail.learn(make_equation_with(backend, 4, {0, 4}), ClauseId{0}); // x0 = 1

    std::vector<Clause> clauses;
    clauses.push_back(make_clause(backend, 4, {{0}}));           // [x0 = 1], already satisfied
    clauses.push_back(make_clause(backend, 4, {{1}, {2}}));      // [x1 = 1] v [x2 = 1]
    clauses.push_back(make_clause(backend, 4, {{0, 1}, {3}}));   // mentions the eliminated x0
    const std::vector<std::uint8_t> active{1, 1, 1};
    const std::vector<std::size_t> var_map{0, 1, 2, 3};

    const CompactionInputs inputs{trail, clauses, active, var_map, 4, false, 0, 0, 16};
    const CompactionTransform transform = build_compaction_transform(trail, 4);
    REQUIRE(transform.new_num_vars() == 3);

    const CompactionPlan plan = build_compaction_plan(inputs, transform);
    REQUIRE(plan.rewritten_clauses.size() == 3);
    REQUIRE(plan.clause_is_active.size() == 3);

    SUBCASE("a satisfied clause becomes an inactive placeholder") {
        CHECK(plan.clause_is_active[0] == 0);
        const Clause& rewritten = plan.rewritten_clauses[0];
        CHECK(rewritten.rows() == 1);
        CHECK(rewritten.cols() == 4);
        CHECK(columns_of(rewritten.row(0)) == std::vector<std::size_t>{3});
        CHECK(rewritten.watched0 == 0);
        CHECK(rewritten.watched1 == 0);
        CHECK(rewritten.next_watch_search_start == 0);
    }

    SUBCASE("an open clause is reindexed and keeps a legal watch pair") {
        CHECK(plan.clause_is_active[1] == 1);
        const Clause& rewritten = plan.rewritten_clauses[1];
        REQUIRE(rewritten.rows() == 2);
        CHECK(columns_of(rewritten.row(0)) == std::vector<std::size_t>{0});
        CHECK(columns_of(rewritten.row(1)) == std::vector<std::size_t>{1});
        CHECK(rewritten.watched0 == 0);
        CHECK(rewritten.watched1 == 1);
    }

    SUBCASE("rows are reduced by the trail before being projected") {
        // x0 + x1 reduces to x1 + 1 against x0 = 1, and x0 is then eliminated.
        CHECK(plan.clause_is_active[2] == 1);
        const Clause& rewritten = plan.rewritten_clauses[2];
        REQUIRE(rewritten.rows() == 2);
        CHECK(columns_of(rewritten.row(0)) == std::vector<std::size_t>{0, 3});
        CHECK(columns_of(rewritten.row(1)) == std::vector<std::size_t>{2});
    }

    SUBCASE("the history entry records the root trail in row-echelon order") {
        REQUIRE(plan.history_entry.trail_rows_before.size() == 1);
        CHECK(columns_of(plan.history_entry.trail_rows_before[0]) ==
              std::vector<std::size_t>{0, 4});
        CHECK(plan.history_entry.active_var_to_original_before == var_map);
        CHECK(plan.history_entry.transform.new_num_vars() == 3);
    }
}

TEST_CASE_TEMPLATE("compaction: merged rows keep a distinct watch pair", Tag, DenseTag,
                   SparseTag) {
    constexpr Algebra backend = Tag::value;
    Trail trail(backend, 4);
    trail.learn(make_equation_with(backend, 4, {0, 1}), ClauseId{0}); // x0 + x1 = 0

    // Both rows reduce to x2, so the rewritten clause has a single row.
    std::vector<Clause> clauses;
    clauses.push_back(make_clause(backend, 4, {{0, 1, 2}, {2}}));
    const std::vector<std::uint8_t> active{1};
    const std::vector<std::size_t> var_map{0, 1, 2, 3};

    const CompactionInputs inputs{trail, clauses, active, var_map, 4, false, 0, 0, 16};
    const CompactionTransform transform = build_compaction_transform(trail, 4);
    const CompactionPlan plan = build_compaction_plan(inputs, transform);

    const Clause& rewritten = plan.rewritten_clauses[0];
    CHECK(rewritten.rows() == 1);
    CHECK(rewritten.watched0 == 0);
    CHECK(rewritten.watched1 == 0);
    CHECK(rewritten.next_watch_search_start == 0);
}

TEST_CASE_TEMPLATE("original space: recovering an assignment", Tag, DenseTag, SparseTag) {
    constexpr Algebra backend = Tag::value;

    SUBCASE("with no compaction the active space is the original space") {
        Bitmap assignment(2);
        assignment.set(0);
        const std::vector<CompactionHistoryEntry> history;
        const std::vector<std::size_t> var_map{2, 5};
        const Bitmap original = reconstruct_original_assignment(assignment, history, var_map, 6);
        CHECK(original.size() == 6);
        CHECK(set_variables(original) == std::vector<std::size_t>{2});
    }

    SUBCASE("one compaction is undone by back-substitution") {
        Trail trail(backend, 4);
        trail.learn(make_equation_with(backend, 4, {0, 4}), ClauseId{0}); // x0 = 1

        const std::vector<Clause> clauses;
        const std::vector<std::uint8_t> active;
        const std::vector<std::size_t> var_map{0, 1, 2, 3};
        const CompactionInputs inputs{trail, clauses, active, var_map, 4, false, 0, 0, 16};
        const CompactionTransform transform = build_compaction_transform(trail, 4);
        const CompactionPlan plan = build_compaction_plan(inputs, transform);

        std::vector<CompactionHistoryEntry> history;
        history.push_back(plan.history_entry);

        // x1 and x3 are true in the compacted space; x0 comes back from the
        // trail row that eliminated it.
        Bitmap compacted(3);
        compacted.set(0);
        compacted.set(2);
        const Bitmap original = reconstruct_original_assignment(compacted, history, var_map, 4);
        CHECK(original.size() == 4);
        CHECK(set_variables(original) == std::vector<std::size_t>{0, 1, 3});
    }

    SUBCASE("back-substitution reads the other variables of the eliminated row") {
        Trail trail(backend, 4);
        trail.learn(make_equation_with(backend, 4, {0, 2}), ClauseId{0}); // x0 + x2 = 0

        const std::vector<Clause> clauses;
        const std::vector<std::uint8_t> active;
        const std::vector<std::size_t> var_map{0, 1, 2, 3};
        const CompactionInputs inputs{trail, clauses, active, var_map, 4, false, 0, 0, 16};
        const CompactionTransform transform = build_compaction_transform(trail, 4);
        const CompactionPlan plan = build_compaction_plan(inputs, transform);
        REQUIRE(transform.new_num_vars() == 3);

        std::vector<CompactionHistoryEntry> history;
        history.push_back(plan.history_entry);

        SUBCASE("x2 false leaves x0 false") {
            const Bitmap compacted(3);
            const Bitmap original = reconstruct_original_assignment(compacted, history, var_map, 4);
            CHECK(set_variables(original).empty());
        }

        SUBCASE("x2 true forces x0 true") {
            Bitmap compacted(3);
            compacted.set(1); // new index 1 is old x2
            const Bitmap original = reconstruct_original_assignment(compacted, history, var_map, 4);
            CHECK(set_variables(original) == std::vector<std::size_t>{0, 2});
        }
    }

    SUBCASE("two compactions are undone newest first") {
        Trail first_trail(backend, 4);
        first_trail.learn(make_equation_with(backend, 4, {0, 4}), ClauseId{0}); // x0 = 1

        const std::vector<Clause> clauses;
        const std::vector<std::uint8_t> active;
        const std::vector<std::size_t> first_map{0, 1, 2, 3};
        const CompactionInputs first_inputs{first_trail, clauses, active, first_map, 4,
                                            false,       0,       0,      16};
        const CompactionTransform first_transform = build_compaction_transform(first_trail, 4);
        const CompactionPlan first_plan = build_compaction_plan(first_inputs, first_transform);

        // After the first step the active variables are old x1, x2, x3.
        const std::vector<std::size_t> second_map{1, 2, 3};
        Trail second_trail(backend, 3);
        second_trail.learn(make_equation_with(backend, 3, {0, 1}), ClauseId{0}); // x1 + x2 = 0
        const CompactionInputs second_inputs{second_trail, clauses, active, second_map, 3,
                                             true,         16,      0,      16};
        const CompactionTransform second_transform = build_compaction_transform(second_trail, 3);
        const CompactionPlan second_plan = build_compaction_plan(second_inputs, second_transform);
        REQUIRE(second_transform.new_num_vars() == 2);

        std::vector<CompactionHistoryEntry> history;
        history.push_back(first_plan.history_entry);
        history.push_back(second_plan.history_entry);

        // Set old x2, which is index 0 of the twice-compacted space.
        Bitmap compacted(2);
        compacted.set(0);
        const Bitmap original = reconstruct_original_assignment(compacted, history, second_map, 4);
        CHECK(original.size() == 4);
        // x0 = 1 from the first step, x2 set directly, x1 = x2 from the second.
        CHECK(set_variables(original) == std::vector<std::size_t>{0, 1, 2});
    }
}

} // namespace xorcle
