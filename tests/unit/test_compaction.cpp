#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <vector>

#include "algebra/compaction_transform.h"
#include "algebra/row_echelon_matrix.h"
#include "algebra_test_support.h"
#include "core/clause_id.h"
#include "core/trail.h"
#include "heuristics/compaction/compaction.h"
#include "heuristics/compaction/original_variables.h"
#include "heuristics_test_support.h"
#include "util/index_set.h"

namespace xorcle {

namespace {

std::vector<std::size_t> set_variables(const IndexSet& assignment) {
    std::vector<std::size_t> out;
    assignment.for_each_set([&](std::size_t index) { out.push_back(index); });
    return out;
}

/// @brief The ids `build_substitution_plan()` takes when every clause is active.
std::vector<ClauseId> candidate_ids_of(const std::vector<Clause>& clauses, const Trail& trail) {
    std::vector<ClauseId> out;
    for (ClauseId id = 0; id < clauses.size(); ++id) {
        if (clauses[id].pivot_col_idxs().test(clauses[id].cols() - 1) ||
            clauses[id].carried_col_idxs().intersects(trail.span().pivot_col_idxs())) {
            out.push_back(id);
        }
    }
    return out;
}

/// @brief The history entry of a substitution by `trail` that a renumbering followed.
CompactionHistoryEntry renumbered_entry(const Trail& trail) {
    const std::vector<Clause> clauses;
    const std::vector<ClauseId> ids;
    const CompactionState state{trail, clauses, ids, trail.num_vars()};
    CompactionHistoryEntry entry = build_substitution_plan(state).history_entry;
    entry.renumbering = CompactionTransform::from_dropped_columns(trail.num_vars(),
                                                                  trail.span().pivot_col_idxs());
    return entry;
}

} // namespace

TEST_CASE_TEMPLATE("compaction: when it should run", Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    Trail trail(repr, 4);
    const std::vector<Clause> clauses;
    const std::vector<ClauseId> ids;

    const CompactionState state{trail, clauses, ids, 4};

    SUBCASE("an empty root trail has nothing to eliminate") {
        CHECK_FALSE(should_compact_before_decision(state));
    }

    SUBCASE("a non-empty root trail is compactable") {
        trail.learn(make_equation_with(repr, 4, {1, 4}), ClauseId{0}, trail.level());
        CHECK(should_compact_before_decision(state));
    }

    SUBCASE("only the root level is compactable") {
        trail.learn(make_equation_with(repr, 4, {1, 4}), ClauseId{0}, trail.level());
        trail.learn(make_equation_with(repr, 4, {2}), std::nullopt, trail.level() + 1);
        REQUIRE(trail.level() == 1);
        CHECK_FALSE(should_compact_before_decision(state));
    }
}

TEST_CASE("compaction: a renumbering waits until it frees a word and 5% of the words") {
    // 63 variables and the constant fill one word; 64 need two.
    CHECK_FALSE(renumbering_frees_enough_words(63, 0));
    CHECK_FALSE(renumbering_frees_enough_words(63, 62));
    CHECK_FALSE(renumbering_frees_enough_words(64, 0));
    CHECK(renumbering_frees_enough_words(64, 1));
    CHECK_FALSE(renumbering_frees_enough_words(130, 2));
    CHECK(renumbering_frees_enough_words(130, 3));
    CHECK(renumbering_frees_enough_words(130, 130));

    // 1279 variables and the constant fill 20 words, of which one is 5%.
    CHECK_FALSE(renumbering_frees_enough_words(1279, 63));
    CHECK(renumbering_frees_enough_words(1279, 64));
    // 1280 need 21, of which one is less than 5% and two are more.
    CHECK_FALSE(renumbering_frees_enough_words(1280, 1));
    CHECK_FALSE(renumbering_frees_enough_words(1280, 64));
    CHECK(renumbering_frees_enough_words(1280, 65));
}

TEST_CASE_TEMPLATE("compaction: rewriting the problem", Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    Trail trail(repr, 4);
    trail.learn(make_equation_with(repr, 4, {0, 4}), ClauseId{0}, trail.level()); // x0 = 1

    std::vector<Clause> clauses;
    clauses.push_back(make_clause(repr, 4, {{0}}));           // [x0 = 1], already satisfied
    clauses.push_back(make_clause(repr, 4, {{1}, {2}}));      // [x1 = 1] v [x2 = 1]
    clauses.push_back(make_clause(repr, 4, {{0, 1}, {3}}));   // mentions the eliminated x0
    const std::vector<ClauseId> ids = candidate_ids_of(clauses, trail);

    const CompactionState state{trail, clauses, ids, 4};
    const SubstitutionPlan plan = build_substitution_plan(state);
    REQUIRE(plan.rewritten_clause_ids == std::vector<ClauseId>{0, 2});
    REQUIRE(plan.rewritten_clauses.size() == 2);
    REQUIRE(plan.rewritten_is_active == std::vector<std::uint8_t>{0, 1});

    SUBCASE("a satisfied clause becomes an inactive placeholder") {
        const Clause& rewritten = plan.rewritten_clauses[0];
        CHECK(rewritten.rows() == 1);
        CHECK(rewritten.cols() == 5);
        CHECK(bits_of(rewritten.row(0)) == std::vector<std::size_t>{4});
        CHECK(rewritten.watched0 == 0);
        CHECK(rewritten.watched1 == 0);
        CHECK(rewritten.next_watch_search_start == 0);
    }

    SUBCASE("rows are reduced by the trail, in the same numbering") {
        // x0 + x1 reduces to x1 + 1 against x0 = 1, which leaves column 0 zero.
        const Clause& rewritten = plan.rewritten_clauses[1];
        REQUIRE(rewritten.rows() == 2);
        CHECK(rewritten.cols() == 5);
        CHECK(bits_of(rewritten.row(0)) == std::vector<std::size_t>{1, 4});
        CHECK(bits_of(rewritten.row(1)) == std::vector<std::size_t>{3});
    }

    SUBCASE("the history entry records the root trail in row-echelon order") {
        REQUIRE(plan.history_entry.trail_rows_before.size() == 1);
        CHECK(bits_of(plan.history_entry.trail_rows_before[0]) ==
              std::vector<std::size_t>{0, 4});
        CHECK_FALSE(plan.history_entry.renumbering.has_value());
    }
}

TEST_CASE_TEMPLATE("compaction: a clause that is no candidate is left out of the plan", Tag,
                   DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    Trail trail(repr, 4);
    trail.learn(make_equation_with(repr, 4, {0, 4}), ClauseId{0}, trail.level()); // x0 = 1

    // The second clause stands for an inactive one, which is never a candidate.
    std::vector<Clause> clauses;
    clauses.push_back(make_clause(repr, 4, {{0, 1}, {2}}));
    clauses.push_back(make_clause(repr, 4, {{0, 2}, {3}}));
    clauses.push_back(make_clause(repr, 4, {{1}, {3}}));
    const std::vector<ClauseId> ids{0};

    const CompactionState state{trail, clauses, ids, 4};
    const SubstitutionPlan plan = build_substitution_plan(state);

    REQUIRE(plan.rewritten_clause_ids == std::vector<ClauseId>{0});
    REQUIRE(plan.rewritten_clauses.size() == 1);
    CHECK(bits_of(plan.rewritten_clauses[0].row(0)) == std::vector<std::size_t>{1, 4});
    CHECK(plan.rewritten_is_active == std::vector<std::uint8_t>{1});
}

TEST_CASE_TEMPLATE("compaction: merged rows keep a distinct watch pair", Tag, DenseTag,
                   SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    Trail trail(repr, 4);
    trail.learn(make_equation_with(repr, 4, {0, 1}), ClauseId{0}, trail.level()); // x0 + x1 = 0

    // Both rows reduce to x2, so the rewritten clause has a single row.
    std::vector<Clause> clauses;
    clauses.push_back(make_clause(repr, 4, {{0, 1, 2}, {2}}));
    const std::vector<ClauseId> ids = candidate_ids_of(clauses, trail);

    const CompactionState state{trail, clauses, ids, 4};
    const SubstitutionPlan plan = build_substitution_plan(state);

    const Clause& rewritten = plan.rewritten_clauses[0];
    CHECK(rewritten.rows() == 1);
    CHECK(rewritten.watched0 == 0);
    CHECK(rewritten.watched1 == 0);
    CHECK(rewritten.next_watch_search_start == 0);
}

TEST_CASE_TEMPLATE("original space: recovering an assignment", Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;

    SUBCASE("with no compaction the active space is the original space") {
        IndexSet assignment(2);
        assignment.set(0);
        const std::vector<CompactionHistoryEntry> history;
        const std::vector<std::size_t> var_map{2, 5};
        const IndexSet original = reconstruct_original_assignment(assignment, history, var_map, 6);
        CHECK(original.size() == 6);
        CHECK(set_variables(original) == std::vector<std::size_t>{2});
    }

    SUBCASE("a renumbered compaction is undone by back-substitution") {
        Trail trail(repr, 4);
        trail.learn(make_equation_with(repr, 4, {0, 4}), ClauseId{0}, trail.level()); // x0 = 1
        const std::vector<CompactionHistoryEntry> history{renumbered_entry(trail)};
        const std::vector<std::size_t> var_map{1, 2, 3};

        // x1 and x3 are true in the compacted space; x0 comes back from the
        // trail row that eliminated it.
        IndexSet compacted(3);
        compacted.set(0);
        compacted.set(2);
        const IndexSet original = reconstruct_original_assignment(compacted, history, var_map, 4);
        CHECK(original.size() == 4);
        CHECK(set_variables(original) == std::vector<std::size_t>{0, 1, 3});
    }

    SUBCASE("back-substitution reads the other variables of the eliminated row") {
        Trail trail(repr, 4);
        trail.learn(make_equation_with(repr, 4, {0, 2}), ClauseId{0}, trail.level()); // x0 + x2 = 0
        const std::vector<CompactionHistoryEntry> history{renumbered_entry(trail)};
        const std::vector<std::size_t> var_map{1, 2, 3};

        SUBCASE("x2 false leaves x0 false") {
            const IndexSet compacted(3);
            const IndexSet original = reconstruct_original_assignment(compacted, history, var_map, 4);
            CHECK(set_variables(original).empty());
        }

        SUBCASE("x2 true forces x0 true") {
            IndexSet compacted(3);
            compacted.set(1); // new index 1 is old x2
            const IndexSet original = reconstruct_original_assignment(compacted, history, var_map, 4);
            CHECK(set_variables(original) == std::vector<std::size_t>{0, 2});
        }
    }

    SUBCASE("a substitution with no renumbering overwrites its dropped variable in place") {
        Trail trail(repr, 4);
        trail.learn(make_equation_with(repr, 4, {1, 3, 4}), ClauseId{0}, trail.level()); // x1 = x3 + 1
        std::vector<CompactionHistoryEntry> history{renumbered_entry(trail)};
        history.back().renumbering.reset();
        const std::vector<std::size_t> var_map{0, 1, 2, 3};

        // The dropped x1 is still in the numbering, and whatever it holds is ignored.
        IndexSet active(4);
        active.set(1);
        active.set(3);
        const IndexSet original = reconstruct_original_assignment(active, history, var_map, 4);
        CHECK(set_variables(original) == std::vector<std::size_t>{3});
    }

    SUBCASE("two renumbered compactions are undone newest first") {
        Trail first_trail(repr, 4);
        // x0 = 1
        first_trail.learn(make_equation_with(repr, 4, {0, 4}), ClauseId{0},
                          first_trail.level());
        // After the first step the active variables are old x1, x2, x3.
        Trail second_trail(repr, 3);
        // x1 + x2 = 0
        second_trail.learn(make_equation_with(repr, 3, {0, 1}), ClauseId{0},
                           second_trail.level());
        const std::vector<CompactionHistoryEntry> history{renumbered_entry(first_trail),
                                                          renumbered_entry(second_trail)};
        const std::vector<std::size_t> second_map{2, 3};

        // Set old x2, which is index 0 of the twice-compacted space.
        IndexSet compacted(2);
        compacted.set(0);
        const IndexSet original = reconstruct_original_assignment(compacted, history, second_map, 4);
        CHECK(original.size() == 4);
        // x0 = 1 from the first step, x2 set directly, x1 = x2 from the second.
        CHECK(set_variables(original) == std::vector<std::size_t>{0, 1, 2});
    }

    SUBCASE("one renumbering undoes the substitutions since the last") {
        Trail first_trail(repr, 4);
        // x0 = 1
        first_trail.learn(make_equation_with(repr, 4, {0, 4}), ClauseId{0},
                          first_trail.level());
        Trail second_trail(repr, 4);
        // x1 + x2 = 0, with x0 still in the numbering
        second_trail.learn(make_equation_with(repr, 4, {1, 2}), ClauseId{0},
                           second_trail.level());
        std::vector<CompactionHistoryEntry> history{renumbered_entry(first_trail),
                                                    renumbered_entry(second_trail)};
        history[0].renumbering.reset();
        IndexSet dropped(5);
        dropped.set(0);
        dropped.set(1);
        history[1].renumbering = CompactionTransform::from_dropped_columns(4, dropped);
        const std::vector<std::size_t> var_map{2, 3};

        IndexSet compacted(2);
        compacted.set(0);
        const IndexSet original = reconstruct_original_assignment(compacted, history, var_map, 4);
        CHECK(set_variables(original) == std::vector<std::size_t>{0, 1, 2});
    }
}

TEST_CASE_TEMPLATE("compaction: a plan over three words rewrites every candidate", Tag,
                   DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    constexpr std::size_t num_vars = 130;
    constexpr std::size_t constant = num_vars;

    SUBCASE("one root unit") {
        Trail trail(repr, num_vars);
        trail.learn(make_equation_with(repr, num_vars, {100, constant}), ClauseId{0},
                    trail.level()); // x100 = 1
        std::vector<Clause> clauses;
        clauses.push_back(make_clause(repr, num_vars, {{100}, {3}}));      // satisfied
        clauses.push_back(make_clause(repr, num_vars, {{5, 100}, {7}}));   // holds x100
        clauses.push_back(make_clause(repr, num_vars, {{5}, {120}}));      // untouched
        clauses.push_back(make_clause(repr, num_vars, {{64}, {129}}));     // untouched
        const std::vector<ClauseId> ids = candidate_ids_of(clauses, trail);

        const CompactionState state{trail, clauses, ids, num_vars};
        const SubstitutionPlan plan = build_substitution_plan(state);

        CHECK(plan.rewritten_clause_ids == std::vector<ClauseId>{0, 1});
        CHECK(plan.rewritten_is_active == std::vector<std::uint8_t>{0, 1});
        // x5 + x100 reduced by x100 = 1 is x5 = 1.
        REQUIRE(plan.rewritten_clauses.size() == 2);
        CHECK(rows_of(plan.rewritten_clauses[1]) ==
              std::vector<std::vector<std::size_t>>{{5, constant}, {7}});
    }

    SUBCASE("three root units over three words") {
        Trail trail(repr, num_vars);
        for (const std::size_t var : {10UL, 70UL, 128UL}) {
            trail.learn(make_equation_with(repr, num_vars, {var}), ClauseId{0}, trail.level());
        }
        std::vector<Clause> clauses;
        clauses.push_back(make_clause(repr, num_vars, {{1}, {2}}));         // untouched
        clauses.push_back(make_clause(repr, num_vars, {{69, 70}, {100}}));  // holds x70
        clauses.push_back(make_clause(repr, num_vars, {{127}, {129}}));     // untouched
        clauses.push_back(make_clause(repr, num_vars, {{0, 128}, {64}}));   // holds x128
        const std::vector<ClauseId> ids = candidate_ids_of(clauses, trail);

        const CompactionState state{trail, clauses, ids, num_vars};
        const SubstitutionPlan plan = build_substitution_plan(state);

        CHECK(plan.rewritten_clause_ids == std::vector<ClauseId>{1, 3});
        CHECK(plan.rewritten_is_active == std::vector<std::uint8_t>{1, 1});
    }
}

} // namespace xorcle
