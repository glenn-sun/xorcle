#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <utility>
#include <vector>

#include "algebra/equation_repr.h"
#include "algebra/row_echelon_matrix.h"
#include "core/clause_id.h"
#include "heuristics/bve/bve.h"
#include "heuristics/compaction/compaction.h"
#include "heuristics/compaction/original_variables.h"
#include "heuristics_test_support.h"
#include "util/index_set.h"
#include "util/random.h"

namespace xorcle {

namespace {

constexpr EquationRepr repr = EquationRepr::dense;

/// @brief An equation: the variables it sums and the value it says the sum has.
struct Disjunct {
    std::vector<std::size_t> vars;
    bool value;
};

/// @brief The clause that is the disjunction of @p disjuncts.
Clause clause_of(std::size_t num_vars, const std::vector<Disjunct>& disjuncts) {
    Clause clause(repr, num_vars);
    for (const Disjunct& disjunct : disjuncts) {
        // Stored as the linear negation: the equation with its constant flipped.
        std::vector<std::size_t> bits = disjunct.vars;
        if (!disjunct.value) {
            bits.push_back(num_vars);
        }
        clause.add_row(Equation::from_bits(repr, num_vars, bits));
    }
    return clause;
}

/// @brief Whether @p assignment satisfies @p clause: some row of its linear negation fails.
bool satisfies(const Clause& clause, const IndexSet& assignment, std::size_t num_vars) {
    for (std::size_t row_idx = 0; row_idx < clause.rows(); ++row_idx) {
        bool sum = false;
        bool constant = false;
        for (const std::size_t bit : clause.row(row_idx).bits()) {
            if (bit == num_vars) {
                constant = true;
            } else {
                sum = sum != assignment.test(bit);
            }
        }
        if (sum != constant) {
            return true;
        }
    }
    return false;
}

constexpr Seed num_seeds = 20;

BvePlan plan_for(const std::vector<Clause>& clauses, std::size_t num_vars,
                 const BveConfig& config = default_bve_config(), Seed seed = default_seed,
                 EquationRepr in_repr = repr) {
    std::vector<ClauseId> ids(clauses.size());
    std::iota(ids.begin(), ids.end(), ClauseId{0});
    const std::vector<std::uint8_t> is_active(clauses.size(), 1);
    Rng rng = make_rng(seed, RngComponent::bve_draw);
    return build_bve_plan(clauses, ActiveClauseIds(ids, is_active), num_vars, in_repr, config, true, rng);
}

/// @brief @p clauses with their rows in representation @p to.
std::vector<Clause> in_repr(const std::vector<Clause>& clauses, std::size_t num_vars, EquationRepr to) {
    std::vector<Clause> out;
    for (const Clause& clause : clauses) {
        Clause& copy = out.emplace_back(to, num_vars);
        for (std::size_t row_idx = 0; row_idx < clause.rows(); ++row_idx) {
            copy.add_row(Equation::from_bits(to, num_vars, clause.row(row_idx).bits()));
        }
    }
    return out;
}

/// @brief Check that @p clauses eliminate in sparse equations as they do in dense ones, step for step.
void check_sparse_plan(const BvePlan& dense, const std::vector<Clause>& clauses, std::size_t num_vars,
                       const BveConfig& config, Seed seed) {
    const BvePlan sparse =
        plan_for(in_repr(clauses, num_vars, EquationRepr::sparse), num_vars, config, seed, EquationRepr::sparse);
    CHECK(sparse.found_empty_clause == dense.found_empty_clause);
    CHECK(sparse.removed_ids == dense.removed_ids);
    REQUIRE(sparse.eliminations.size() == dense.eliminations.size());
    for (std::size_t i = 0; i < dense.eliminations.size(); ++i) {
        CHECK(sparse.eliminations[i].flip_set == dense.eliminations[i].flip_set);
        CHECK(sparse.eliminations[i].removed_clauses == dense.eliminations[i].removed_clauses);
    }
    REQUIRE(sparse.resolvents.size() == dense.resolvents.size());
    for (std::size_t i = 0; i < dense.resolvents.size(); ++i) {
        CHECK(sparse.resolvents[i].clause.has_value() == dense.resolvents[i].clause.has_value());
        if (sparse.resolvents[i].clause.has_value() && dense.resolvents[i].clause.has_value()) {
            REQUIRE(sparse.resolvents[i].clause->rows() == dense.resolvents[i].clause->rows());
            for (std::size_t row_idx = 0; row_idx < dense.resolvents[i].clause->rows(); ++row_idx) {
                CHECK(sparse.resolvents[i].clause->row(row_idx).bits() ==
                      dense.resolvents[i].clause->row(row_idx).bits());
            }
        }
    }
    CHECK(sparse.counters.clauses_after == dense.counters.clauses_after);
    CHECK(sparse.counters.width_after == dense.counters.width_after);
}

/// @brief The clauses a plan leaves of @p clauses.
std::vector<Clause> left_by(const BvePlan& plan, const std::vector<Clause>& clauses) {
    std::vector<Clause> left;
    for (ClauseId id = 0; id < clauses.size(); ++id) {
        if (!std::ranges::binary_search(plan.removed_ids, id)) {
            left.push_back(clauses[id]);
        }
    }
    for (const BvePlan::Resolvent& resolvent : plan.resolvents) {
        if (resolvent.clause.has_value()) {
            left.push_back(*resolvent.clause);
        }
    }
    return left;
}

/// @brief Check, over every assignment, that one satisfying the clauses the
/// plan leaves is completed to one satisfying @p clauses, and that the two
/// formulas are satisfiable together.
void check_models(const BvePlan& plan, const std::vector<Clause>& clauses, std::size_t num_vars) {
    const std::vector<Clause> left = left_by(plan, clauses);
    const std::vector<CompactionHistoryEntry> history{CompactionHistoryEntry{
        .eliminations = plan.eliminations, .renumbering = std::nullopt}};
    std::vector<std::size_t> every_var(num_vars);
    std::iota(every_var.begin(), every_var.end(), std::size_t{0});
    const auto satisfies_all = [num_vars](const std::vector<Clause>& formula,
                                          const IndexSet& assignment) {
        return std::ranges::all_of(formula, [&](const Clause& clause) {
            return satisfies(clause, assignment, num_vars);
        });
    };
    bool left_is_satisfiable = false;
    bool original_is_satisfiable = false;
    for (std::size_t point = 0; point < (std::size_t{1} << num_vars); ++point) {
        IndexSet assignment(num_vars);
        for (std::size_t var = 0; var < num_vars; ++var) {
            if (((point >> var) & 1U) != 0) {
                assignment.set(var);
            }
        }
        original_is_satisfiable = original_is_satisfiable || satisfies_all(clauses, assignment);
        if (!satisfies_all(left, assignment)) {
            continue;
        }
        left_is_satisfiable = true;
        const IndexSet completed =
            reconstruct_original_assignment(assignment, history, every_var, num_vars);
        CHECK(satisfies_all(clauses, completed));
    }
    CHECK(left_is_satisfiable == (original_is_satisfiable && !plan.found_empty_clause));
}

/// @brief [x = 1] v [y = 1], [x = 0] v [z = 1] and [y = 0] v [w = 1] over x, y, z, w = 0, 1, 2, 3.
std::vector<Clause> example_7_2(std::size_t num_vars) {
    return {clause_of(num_vars, {{{0}, true}, {{1}, true}}),
            clause_of(num_vars, {{{0}, false}, {{2}, true}}),
            clause_of(num_vars, {{{1}, false}, {{3}, true}})};
}

/// @brief The gate x4 = x3 + x1 x2 in three clauses and two clauses reading x4, over x1..x4 = 0..3, and
/// [x1 = 1] v [x2 = 1].
std::vector<Clause> example_7_3(std::size_t num_vars) {
    return {clause_of(num_vars, {{{0}, true}, {{2, 3}, false}}),
            clause_of(num_vars, {{{1}, true}, {{2, 3}, false}}),
            clause_of(num_vars, {{{0}, false}, {{1}, false}, {{2, 3}, true}}),
            clause_of(num_vars, {{{1, 2}, true}, {{3}, false}}),
            clause_of(num_vars, {{{2}, true}, {{3}, true}}),
            clause_of(num_vars, {{{0}, true}, {{1}, true}})};
}

/// @brief The paper's example over x, y, a = 0, 1, 2, [x = 1] v [a = 1], [y = 1] v [a = 0] and
/// [x + y = 0] v [a + y = 1], with [x = 1] v [y = 1] and [x = 0] v [y = 0] so that every other
/// choice touches more rows than flipping a, which touches the three.
std::vector<Clause> paper_example(std::size_t num_vars) {
    return {clause_of(num_vars, {{{0}, true}, {{2}, true}}), clause_of(num_vars, {{{1}, true}, {{2}, false}}),
            clause_of(num_vars, {{{0, 1}, false}, {{1, 2}, true}}), clause_of(num_vars, {{{0}, true}, {{1}, true}}),
            clause_of(num_vars, {{{0}, false}, {{1}, false}})};
}

} // namespace

TEST_CASE("bve: a variable defined by a gate is eliminated through the gate") {
    // y = a and b, read by two more clauses; y is variable 0.
    constexpr std::size_t num_vars = 5;
    std::vector<Clause> clauses{
        clause_of(num_vars, {{{0}, false}, {{1}, true}}),
        clause_of(num_vars, {{{0}, false}, {{2}, true}}),
        clause_of(num_vars, {{{0}, true}, {{1}, false}, {{2}, false}}),
        clause_of(num_vars, {{{0}, true}, {{3}, true}}),
        clause_of(num_vars, {{{0}, false}, {{4}, true}})};
    // Seven sign patterns over each of three triples, which no flip set of the other variables touches
    // without touching many clauses.
    for (const auto& triple : std::vector<std::vector<std::size_t>>{{1, 2, 3}, {1, 2, 4}, {2, 3, 4}}) {
        for (std::size_t signs = 1; signs < 8; ++signs) {
            clauses.push_back(clause_of(num_vars, {{{triple[0]}, (signs & 1U) != 0},
                                                   {{triple[1]}, (signs & 2U) != 0},
                                                   {{triple[2]}, (signs & 4U) != 0}}));
        }
    }

    // y's clauses are more than the default share of these few.
    BveConfig config = default_bve_config();
    config.gate_max_clause_share = 1.0;
    for (Seed seed = 0; seed < num_seeds; ++seed) {
        const BvePlan plan = plan_for(clauses, num_vars, config, seed);
        const auto of_y = std::ranges::find_if(plan.eliminations, [](const Elimination& elimination) {
            return elimination.flip_set == std::vector<std::uint32_t>{0};
        });
        REQUIRE(of_y != plan.eliminations.end());
        CHECK(plan.counters.num_gates > 0);
        CHECK(of_y->removed_clauses.size() == 5);
        CHECK(plan.dropped_col_idxs.test(0));
        check_models(plan, clauses, num_vars);
    }
}

TEST_CASE("bve: two clauses that contradict on one row resolve to the empty clause") {
    constexpr std::size_t num_vars = 2;
    const std::vector<Clause> clauses{clause_of(num_vars, {{{0, 1}, true}}),
                                      clause_of(num_vars, {{{0, 1}, false}})};

    const BvePlan plan = plan_for(clauses, num_vars);
    CHECK(plan.found_empty_clause);
    REQUIRE(plan.resolvents.size() == 1);
    REQUIRE(plan.resolvents.front().clause.has_value());
    CHECK(plan.resolvents.front().clause->rows() == 0);
}

TEST_CASE("bve: equations over many words eliminate what equations over one word do") {
    for (Seed seed = 0; seed < num_seeds; ++seed) {
        const BvePlan one_word = plan_for(example_7_3(4), 4, default_bve_config(), seed);
        const BvePlan many_words = plan_for(example_7_3(400), 400, default_bve_config(), seed);
        REQUIRE(one_word.eliminations.size() == many_words.eliminations.size());
        for (std::size_t i = 0; i < one_word.eliminations.size(); ++i) {
            CHECK(one_word.eliminations[i].flip_set == many_words.eliminations[i].flip_set);
            CHECK(one_word.eliminations[i].removed_clauses == many_words.eliminations[i].removed_clauses);
        }
        CHECK(one_word.resolvents.size() == many_words.resolvents.size());
        CHECK(one_word.counters.clauses_after == many_words.counters.clauses_after);
    }
}

TEST_CASE("bve: sparse equations eliminate what dense equations do") {
    for (Seed seed = 0; seed < num_seeds; ++seed) {
        for (const std::size_t num_vars : {std::size_t{4}, std::size_t{400}}) {
            check_sparse_plan(plan_for(example_7_3(num_vars), num_vars, default_bve_config(), seed), example_7_3(num_vars), num_vars,
                              default_bve_config(), seed);
            check_sparse_plan(plan_for(paper_example(num_vars), num_vars, default_bve_config(), seed), paper_example(num_vars),
                              num_vars, default_bve_config(), seed);
        }
    }
}

TEST_CASE("bve: models are completed on random formulas, in the variables given and in others, sparse as dense") {
    Rng rng = make_rng(7, RngComponent::bve_draw);
    const auto below = [&rng](std::size_t bound) { return static_cast<std::size_t>(rng() % bound); };
    std::size_t num_eliminated = 0;
    std::size_t num_flip_sets_of_several = 0;
    std::size_t num_unit_resolvents = 0;
    for (std::size_t round = 0; round < 300; ++round) {
        const std::size_t num_vars = 4 + below(5);
        // Each variable's image under a change of variables: itself, and for
        // half the formulas other variables added in.
        std::vector<std::vector<std::size_t>> image(num_vars);
        for (std::size_t var = 0; var < num_vars; ++var) {
            image[var] = {var};
        }
        if (round % 2 == 1) {
            for (std::size_t i = 0; i < num_vars; ++i) {
                const std::size_t to = below(num_vars);
                const std::size_t from = below(num_vars);
                if (to == from) {
                    continue;
                }
                std::vector<std::size_t> sum;
                std::ranges::set_symmetric_difference(image[to], image[from],
                                                      std::back_inserter(sum));
                image[to] = std::move(sum);
            }
        }
        std::vector<Clause> clauses;
        const std::size_t num_clauses = 3 + below(3 * num_vars);
        for (std::size_t c = 0; c < num_clauses; ++c) {
            std::vector<Disjunct> disjuncts;
            const std::size_t width = 1 + below(4);
            for (std::size_t d = 0; d < width; ++d) {
                std::vector<std::size_t> vars;
                const std::size_t num_summed = below(4) == 0 ? 2 : 1;
                for (std::size_t s = 0; s < num_summed; ++s) {
                    std::vector<std::size_t> sum;
                    std::ranges::set_symmetric_difference(vars, image[below(num_vars)],
                                                          std::back_inserter(sum));
                    vars = std::move(sum);
                }
                disjuncts.push_back({vars, below(2) == 0});
            }
            Clause clause = clause_of(num_vars, disjuncts);
            clause.reduce_rows();
            // BVE reads no tautology and no two clauses with one span.
            if (!clause.pivot_col_idxs().test(num_vars) &&
                std::ranges::none_of(clauses, [&clause](const Clause& other) { return other.has_rows_of(clause); })) {
                clauses.push_back(std::move(clause));
            }
        }
        // With caps low enough to bind.
        BveConfig config = default_bve_config();
        config.group_cap = 1 + below(4);
        config.max_refusals_in_a_row =
            std::array<std::size_t, 3>{std::numeric_limits<std::size_t>::max(), 1, 3}[below(3)];
        config.gate_max_clause_share = std::array<double, 2>{1.0, config.gate_max_clause_share}[below(2)];
        const BvePlan plan = plan_for(clauses, num_vars, config, round);
        check_sparse_plan(plan, clauses, num_vars, config, round);
        num_eliminated += plan.eliminations.size();
        for (const Elimination& elimination : plan.eliminations) {
            num_flip_sets_of_several += elimination.flip_set.size() > 1 ? 1U : 0U;
        }
        num_unit_resolvents += plan.counters.num_unit_resolvents;
        check_models(plan, clauses, num_vars);
    }
    CHECK(num_eliminated > 100);
    CHECK(num_flip_sets_of_several > 10);
    // A unit resolvent stays among the clauses, for the solver to substitute.
    CHECK(num_unit_resolvents > 10);
}

TEST_CASE("bve: projection finds the projection by a flip whose touched clauses share no direction") {
    constexpr std::size_t num_vars = 3;
    const std::vector<Clause> clauses = paper_example(num_vars);
    for (Seed seed = 0; seed < num_seeds; ++seed) {
        const BvePlan plan = plan_for(clauses, num_vars, default_bve_config(), seed);
        REQUIRE_FALSE(plan.eliminations.empty());
        CHECK(plan.eliminations.front().flip_set == std::vector<std::uint32_t>{2});
        CHECK(plan.eliminations.front().removed_clauses.size() == 3);
        check_models(plan, clauses, num_vars);
    }
}

TEST_CASE("bve: a clause that a flip set touches alone goes") {
    constexpr std::size_t num_vars = 4;
    const std::vector<Clause> clauses = example_7_2(num_vars);
    for (Seed seed = 0; seed < num_seeds; ++seed) {
        const BvePlan plan = plan_for(clauses, num_vars, default_bve_config(), seed);
        CHECK(plan.eliminations.size() == 3);
        CHECK(plan.resolvents.empty());
        CHECK(plan.counters.clauses_after == 0);
        check_models(plan, clauses, num_vars);
    }
}

} // namespace xorcle
