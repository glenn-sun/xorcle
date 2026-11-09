/// @file test_fuzz.cpp
/// @brief Whole runs on random instances, each answer checked independently.

#include <doctest/doctest.h>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "algebra/equation.h"
#include "algebra/equation_repr.h"
#include "algebra/row_echelon_matrix.h"
#include "algebra_test_support.h"
#include "cli/options.h"
#include "core/solver.h"
#include "io/dimacs.h"
#include "io/proof_postprocess.h"
#include "io/proof_raw_file.h"
#include "io_test_support.h"
#include "util/index_set.h"

namespace xorcle {

namespace {

/// @brief A random XNF instance over 6 to 10 variables near its satisfiability
/// threshold: clauses of two or three equations, a few of one, each equation
/// over one to three variables.
std::string random_instance(TestRandom& random) {
    const std::size_t num_vars = 6 + random.next(5);
    const std::size_t num_equations = 2 + random.next(2);
    // A clause of k random equations is falsified by one assignment in 2^k,
    // which puts the threshold near 2.4 clauses per variable for two equations
    // and 5.2 for three.
    const std::size_t per_ten_vars =
        num_equations == 2 ? 20 + random.next(10) : 44 + random.next(16);
    const std::size_t num_clauses = num_vars * per_ten_vars / 10;
    std::string text =
        "p xnf " + std::to_string(num_vars) + " " + std::to_string(num_clauses) + "\n";
    for (std::size_t clause = 0; clause < num_clauses; ++clause) {
        const std::size_t clause_num_equations = random.chance(16) ? 1 : num_equations;
        for (std::size_t eqn = 0; eqn < clause_num_equations; ++eqn) {
            text += random.chance(2) ? "-" : "";
            const std::size_t num_terms = 1 + (random.chance(3) ? 1 + random.next(2) : 0);
            for (std::size_t term = 0; term < num_terms; ++term) {
                text += (term == 0 ? "" : "+") + std::to_string(1 + random.next(num_vars));
            }
            text += " ";
        }
        text += "0\n";
    }
    return text;
}

/// @brief A clause as the assignments that falsify each of its rows' negations:
/// row `i` holds exactly when the parity of `assignment & masks[i]` is
/// `constants[i]`.
struct ClauseMasks {
    std::vector<std::uint64_t> masks;
    std::vector<bool> constants;
};

ClauseMasks masks_of(const Clause& clause) {
    ClauseMasks out;
    for (const Equation& row : clause) {
        std::uint64_t mask = 0;
        bool constant = false;
        for (const std::size_t bit : row.bits()) {
            if (bit == clause.num_vars()) {
                constant = true;
            } else {
                mask |= std::uint64_t{1} << bit;
            }
        }
        out.masks.push_back(mask);
        out.constants.push_back(constant);
    }
    return out;
}

/// @brief Whether an assignment, one bit per variable, satisfies a clause
/// stored as its linear negation: some row must fail to hold.
bool satisfies(const ClauseMasks& clause, std::uint64_t assignment) {
    for (std::size_t i = 0; i < clause.masks.size(); ++i) {
        if ((std::popcount(assignment & clause.masks[i]) % 2 == 1) != clause.constants[i]) {
            return true;
        }
    }
    return false;
}

bool has_model(const std::vector<ClauseMasks>& clauses, std::size_t num_vars) {
    for (std::uint64_t assignment = 0; assignment < (std::uint64_t{1} << num_vars);
         ++assignment) {
        bool all = true;
        for (const ClauseMasks& clause : clauses) {
            if (!satisfies(clause, assignment)) {
                all = false;
                break;
            }
        }
        if (all) {
            return true;
        }
    }
    return false;
}

struct Tally {
    std::size_t sat = 0;
    std::size_t unsat = 0;
    SolverCounters counters; ///< Summed over the runs, for the coverage checks.
    std::size_t units_fed = 0;
    std::size_t subsume_pass_rows_removed = 0;
    std::size_t subsume_pass_units = 0;
    std::size_t subsume_pass_budget_stops = 0;
};

/// @brief Whether an assignment over the original variables satisfies a clause,
/// given by its rows over them.
bool clause_holds(const std::vector<RawRow>& rows, const IndexSet& assignment) {
    for (const RawRow& row : rows) {
        bool parity = false;
        for (const std::size_t bit : row) {
            parity ^= bit == assignment.size() || assignment.test(bit);
        }
        if (parity) {
            return true;
        }
    }
    return false;
}

/// @brief The verdict 2^n enumeration gives.
SolveResult enumerated_verdict(const std::string& text) {
    const ParsedDimacs parsed = parse_dimacs_string(text, EquationRepr::dense);
    std::vector<ClauseMasks> masks;
    masks.reserve(parsed.clauses.size());
    for (const Clause& clause : parsed.clauses) {
        masks.push_back(masks_of(clause));
    }
    return has_model(masks, parsed.num_vars) ? SolveResult::sat : SolveResult::unsat;
}

/// @brief Solve @p text as `xorcle <flags>` would, check a model against the
/// input or a proof with `LrupChecker`, and the verdict against @p expected
/// when there is one.
SolveResult run_and_check(const std::string& text, const std::vector<std::string>& flags,
                          Tally& tally, std::optional<SolveResult> expected) {
    std::vector<std::string> arguments = flags;
    arguments.emplace_back("instance.xnf");
    std::ostringstream help;
    const std::optional<Options> options = parse_options(arguments, help);
    REQUIRE(options.has_value());

    ParsedDimacs parsed = parse_dimacs_string(text, options->equation_repr);
    const std::size_t num_vars = parsed.num_vars;
    const std::vector<std::vector<RawRow>> rows = clause_rows(parsed);

    const std::filesystem::path path = scratch_path("fuzz.raw");
    Solution solution;
    {
        RawProofWriter writer(path, num_vars, rows.size());
        const std::unique_ptr<Solver> solver =
            make_solver(*options, num_vars, parsed.used_vars, &writer);
        for (Clause& clause : parsed.clauses) {
            solver->add_starting_clause(std::move(clause));
        }
        solution = solver->solve();
        writer.finalize(solution.result == SolveResult::unsat);

        const SolverCounters& counters = solver->counters();
        tally.counters.num_conflicts += counters.num_conflicts;
        tally.counters.num_compactions += counters.num_compactions;
        tally.counters.num_deletion_passes += counters.num_deletion_passes;
        tally.counters.num_subsumed_clauses += counters.num_subsumed_clauses;
        tally.counters.num_subsume_replacements += counters.num_subsume_replacements;
        tally.counters.num_equivalent_clauses_at_input += counters.num_equivalent_clauses_at_input;
        tally.counters.num_equivalent_clauses_at_substitution +=
            counters.num_equivalent_clauses_at_substitution;
        tally.counters.num_restarts += counters.num_restarts;
        tally.counters.num_decision_propagations += counters.num_decision_propagations;
        tally.counters.num_undone_decisions += counters.num_undone_decisions;
        tally.units_fed += solver->implication_graph_counters().num_units_fed;
        tally.subsume_pass_rows_removed += solver->subsume_pass_counters().num_rows_removed;
        tally.subsume_pass_units += solver->subsume_pass_counters().num_units;
        tally.subsume_pass_budget_stops += solver->subsume_pass_counters().num_budget_stops;
    }

    REQUIRE(solution.result == expected.value_or(solution.result));
    if (solution.result == SolveResult::sat) {
        ++tally.sat;
        REQUIRE(solution.assignment.has_value());
        REQUIRE(solution.assignment->size() == num_vars);
        for (const std::vector<RawRow>& clause : rows) {
            CHECK(clause_holds(clause, *solution.assignment));
        }
        return solution.result;
    }

    ++tally.unsat;
    RawProofFileSource source(path);
    std::ostringstream proof;
    postprocess_raw_proof(source, num_vars, rows.size(), options->equation_repr, proof, rows);
    LrupChecker checker(options->equation_repr, num_vars, rows);
    CHECK(checker.check(proof.str()) == "");
    std::filesystem::remove(path);
    return solution.result;
}

/// @brief A random XNF instance of two-equation clauses over 70 to 100
/// variables, two or three words a row, near its satisfiability threshold.
std::string random_wide_instance(TestRandom& random) {
    const std::size_t num_vars = 70 + random.next(31);
    const std::size_t num_clauses = num_vars * (18 + random.next(5)) / 10;
    std::string text =
        "p xnf " + std::to_string(num_vars) + " " + std::to_string(num_clauses) + "\n";
    for (std::size_t clause = 0; clause < num_clauses; ++clause) {
        for (std::size_t eqn = 0; eqn < 2; ++eqn) {
            text += random.chance(2) ? "-" : "";
            const std::size_t num_terms = 1 + (random.chance(3) ? 1 + random.next(3) : 0);
            for (std::size_t term = 0; term < num_terms; ++term) {
                text += (term == 0 ? "" : "+") + std::to_string(1 + random.next(num_vars));
            }
            text += " ";
        }
        text += "0\n";
    }
    return text;
}

} // namespace

// Too wide to enumerate, so each of the exact watchlist's answers is held to its
// model or its proof. Deletion, subsumption and compaction take clauses out of
// and back into the watchlist as it runs; in Debug its cells are checked
// against the trail along the way, and each quiescence against every clause.
TEST_CASE("fuzz: past one word, the exact watchlist's answers check") {
    TestRandom random(1234);
    Tally tally;
    for (std::size_t index = 0; index < 10; ++index) {
        const std::string text = random_wide_instance(random);
        CAPTURE(index);
        CAPTURE(text);
        run_and_check(text, {"--deletion-cadence-coefficient=0.5", "--deletion-cadence-offset=0"}, tally, std::nullopt);
    }
    CHECK(tally.sat > 0);
    CHECK(tally.unsat > 0);
    CHECK(tally.counters.num_compactions > 0);
    CHECK(tally.counters.num_deletion_passes > 0);
    CHECK(tally.counters.num_subsumed_clauses > 0);
}

// Each configuration runs every instance, so a failure names both; the seeds
// are fixed, so it reproduces.
TEST_CASE("fuzz: random instances agree with brute force, and every answer checks") {
    /// @brief A command line, and what it must be seen doing over the instances.
    struct Configuration {
        std::string name;
        std::vector<std::string> flags;
        std::size_t SolverCounters::*exercised = nullptr;
    };
    // Deletion passes and the graph are due every few conflicts.
    const std::vector<std::string> often = {"--deletion-cadence-coefficient=0.1",
                                            "--deletion-cadence-offset=0",
                                            "--deletion-wait=0",
                                            "--implication-graph-cadence=4"};
    std::vector<std::string> often_without_graph(often.begin(), often.end() - 1);
    often_without_graph.emplace_back("--implication-graph=false");
    const std::vector<Configuration> configurations = {
        {"defaults", {}, &SolverCounters::num_subsume_replacements},
        {"defaults, often", often, &SolverCounters::num_deletion_passes},
        {"no graph, often", often_without_graph, &SolverCounters::num_deletion_passes},
        {"cyclic, complete", {"--selection=cyclic", "--propagate-miss-rate=0"}},
        {"cyclic, miss rate 0.05",
         {"--selection=cyclic", "--propagate-window=4"},
         &SolverCounters::num_decision_propagations},
        {"cyclic, sparse",
         {"--selection=cyclic", "--equation-repr=sparse", "--propagate-miss-rate=0.5",
          "--propagate-window=1", "--csids-intersection-depth=8"},
         &SolverCounters::num_undone_decisions},
        {"luby",
         {"--restart=luby", "--restart-luby-unit=1"},
         &SolverCounters::num_restarts},
        {"span plus-minus", {"--csids-span=plus_minus"}},
        {"span plus-minus, random side", {"--csids-span=plus_minus", "--phase=random"}},
        {"phase saving", {"--phase=saved"}},
        {"phase saving, span plus-minus", {"--phase=saved", "--csids-span=plus_minus"}},
        {"phase saving, two recent trails, target reset often",
         {"--phase=saved", "--phase-saving-recent=2", "--phase-saving-target-reset=10"}},
        {"phase saving, top clause alone",
         {"--phase=saved", "--csids-intersection-depth=1"}},
        {"phase saving, luby restarts, no recent trail",
         {"--phase=saved", "--restart=luby", "--phase-saving-recent=0"}},
        {"phase saving, no target, sparse",
         {"--phase=saved", "--phase-saving-target=false", "--selection=cyclic",
          "--equation-repr=sparse"}},
        {"phase saving, target replaced on ties",
         {"--phase=saved", "--phase-saving-target-on-ties=true", "--phase-saving-target-reset=1"}},
        {"phase rotation, short steps", {"--phase=rotate", "--phase-rotate-interval=2"}},
        {"phase rotation, span plus-minus",
         {"--phase=rotate", "--phase-rotate-interval=3", "--csids-span=plus_minus"}},
        {"phase rotation, no target",
         {"--phase=rotate", "--phase-rotate-interval=1", "--phase-saving-target=false"}},
        {"stable and focused modes, short phases", {"--stabilize=true", "--stabilize-interval=2"}},
        {"stable and focused modes under rotation",
         {"--stabilize=true", "--stabilize-interval=3", "--phase=rotate",
          "--phase-rotate-interval=2"}},
        {"stable and focused modes, trail blocking, slow average 1000",
         {"--stabilize=true", "--stabilize-interval=1", "--focused-restart-lbd-slow=1000",
          "--focused-restart-lbd-min-conflicts=3", "--focused-restart-lbd-trail-ratio=1.4"}},
    };

    // Equivalent clauses are evicted at input and at a substitution; at a subsumption
    // only on instances larger than these, which a roundtrip test covers.
    SolverCounters evictions;
    for (const Configuration& configuration : configurations) {
        CAPTURE(configuration.name);
        TestRandom random(20260925);
        Tally tally;
        for (std::size_t index = 0; index < 50; ++index) {
            const std::string text = random_instance(random);
            CAPTURE(index);
            CAPTURE(text);
            std::vector<std::string> flags = configuration.flags;
            flags.push_back("--seed=" + std::to_string(index % 3));
            run_and_check(text, flags, tally, enumerated_verdict(text));
        }
        // The instances are only worth something if both verdicts and the
        // search features actually occur.
        CHECK(tally.sat > 0);
        CHECK(tally.unsat > 0);
        CHECK(tally.counters.num_conflicts > 0);
        CHECK(tally.counters.num_compactions > 0);
        CHECK(tally.counters.num_subsumed_clauses > 0);
        const bool graph_off = std::ranges::find(configuration.flags,
                                                 "--implication-graph=false") !=
                               configuration.flags.end();
        CHECK((tally.units_fed > 0) != graph_off);
        if (configuration.exercised != nullptr) {
            CHECK(tally.counters.*configuration.exercised > 0);
        }
        evictions.num_equivalent_clauses_at_input += tally.counters.num_equivalent_clauses_at_input;
        evictions.num_equivalent_clauses_at_substitution +=
            tally.counters.num_equivalent_clauses_at_substitution;
    }
    CHECK(evictions.num_equivalent_clauses_at_input > 0);
    CHECK(evictions.num_equivalent_clauses_at_substitution > 0);
}

// Its answers are held to brute force, its models to the input and its proofs,
// each narrowing a line, to the checker.
TEST_CASE("fuzz: vivification leaves the verdict, the models and the proofs right") {
    struct Arm {
        std::string name;
        std::vector<std::string> flags;
        bool stops_at_budget = false;
    };
    // A pass of the default cadence never falls inside instances this small, and
    // the default effort's budget seldom reaches past a few orders.
    const std::vector<Arm> arms = {
        {"new or narrowed, two rounds",
         {"--vivify=true", "--vivify-cadence=0", "--vivify-effort=1000"}},
        {"four rounds, ten conflicts apart",
         {"--vivify=true", "--vivify-cadence=10", "--vivify-rounds=4", "--vivify-effort=1000"}},
        {"one round, a conflict apart",
         {"--vivify=true", "--vivify-cadence=1", "--vivify-rounds=1", "--vivify-effort=1000"}},
        {"forced root visits, one round, a conflict apart",
         {"--vivify=true", "--vivify-cadence=0", "--vivify-rounds=1", "--vivify-effort=1000",
          "--vivify-force-root-visit=true"}},
        {"the default effort",
         {"--vivify=true", "--vivify-cadence=5"},
         true},
    };
    for (const Arm& arm : arms) {
        CAPTURE(arm.name);
        TestRandom random(20261013);
        VivifyCounters total;
        std::size_t num_kept = 0; // Narrowings a kept clause already had.
        std::size_t num_sat = 0;
        std::size_t num_unsat = 0;
        for (std::size_t index = 0; index < 150; ++index) {
            const std::string text = index % 5 == 4 ? random_wide_instance(random)
                                                    : random_instance(random);
            CAPTURE(index);
            CAPTURE(text);
            std::ostringstream help;
            std::vector<std::string> arguments;
            arguments.insert(arguments.end(), arm.flags.begin(), arm.flags.end());
            arguments.push_back("--seed=" + std::to_string(index % 3));
            arguments.emplace_back("instance.xnf");
            const std::optional<Options> options = parse_options(arguments, help);
            REQUIRE(options.has_value());
            ParsedDimacs parsed = parse_dimacs_string(text, options->equation_repr);
            const std::vector<std::vector<RawRow>> rows = clause_rows(parsed);
            const std::filesystem::path path = scratch_path("fuzz_vivify.raw");
            RawProofWriter writer(path, parsed.num_vars, rows.size());
            const std::unique_ptr<Solver> solver =
                make_solver(*options, parsed.num_vars, parsed.used_vars, &writer);
            for (Clause& clause : parsed.clauses) {
                solver->add_starting_clause(std::move(clause));
            }
            const Solution solution = solver->solve();
            writer.finalize(solution.result == SolveResult::unsat);
            if (index % 5 != 4) {
                REQUIRE(solution.result == enumerated_verdict(text));
            }
            if (solution.result == SolveResult::sat) {
                ++num_sat;
                REQUIRE(solution.assignment.has_value());
                for (const std::vector<RawRow>& clause : rows) {
                    CHECK(clause_holds(clause, *solution.assignment));
                }
            } else {
                ++num_unsat;
                RawProofFileSource source(path);
                std::ostringstream proof;
                postprocess_raw_proof(source, parsed.num_vars, rows.size(),
                                      options->equation_repr, proof, rows);
                LrupChecker checker(options->equation_repr, parsed.num_vars, rows);
                CHECK(checker.check(proof.str()) == "");
                std::filesystem::remove(path);
            }
            const VivifyCounters& counters = solver->vivify_counters();
            total.num_orders += counters.num_orders;
            total.num_narrowed += counters.num_narrowed;
            total.num_units += counters.num_units;
            total.num_narrowed_by_refutation += counters.num_narrowed_by_refutation;
            total.num_budget_stops += counters.num_budget_stops;
            num_kept += solver->counters().num_equivalent_clauses_at_vivification;
        }
        MESSAGE(arm.name << ": orders " << total.num_orders << ", narrowed " << total.num_narrowed
                         << ", units " << total.num_units << ", by refutation "
                         << total.num_narrowed_by_refutation << ", budget stops "
                         << total.num_budget_stops << ", a kept clause's "
                         << num_kept << ", sat " << num_sat
                         << ", unsat " << num_unsat);
        CHECK(num_sat > 0);
        CHECK(num_unsat > 0);
        CHECK(total.num_orders > 0);
        CHECK(total.num_narrowed > 0);
        if (arm.stops_at_budget) {
            CHECK(total.num_budget_stops > 0);
        }
    }
}

// The subsumption pass deletes and strengthens clauses with a proof line each, so
// its answers are held to brute force, their models and their proofs; the wide
// instances only to the models and proofs.
TEST_CASE("fuzz: the subsumption pass leaves every answer right and checked") {
    struct Arm {
        std::string name;
        std::vector<std::string> flags;
        bool stops_at_budget = false;
    };
    const std::vector<Arm> arms = {
        {"every root visit, nearly unbudgeted",
         {"--subsume-pass=true", "--subsume-pass-cadence=0", "--subsume-pass-effort=1000000"}},
        {"a conflict apart, nearly unbudgeted",
         {"--subsume-pass=true", "--subsume-pass-cadence=1", "--subsume-pass-effort=1000000"}},
        {"no compaction, a conflict apart, nearly unbudgeted",
         {"--subsume-pass=true", "--subsume-pass-cadence=1", "--compact=false",
          "--subsume-pass-effort=1000000"}},
        {"cyclic with a miss rate, sparse",
         {"--subsume-pass=true", "--subsume-pass-cadence=2", "--selection=cyclic",
          "--equation-repr=sparse", "--propagate-miss-rate=0.5", "--propagate-window=1"}},
        {"a conflict apart, the default effort",
         {"--subsume-pass=true", "--subsume-pass-cadence=1"},
         true},
        {"every root visit, effort 1",
         {"--subsume-pass=true", "--subsume-pass-cadence=0", "--subsume-pass-effort=1"},
         true},
    };
    for (const Arm& arm : arms) {
        CAPTURE(arm.name);
        TestRandom random(20261108);
        Tally tally;
        for (std::size_t index = 0; index < 300; ++index) {
            const bool wide = index % 5 == 4;
            const std::string text = wide ? random_wide_instance(random) : random_instance(random);
            CAPTURE(index);
            CAPTURE(text);
            std::vector<std::string> flags = arm.flags;
            flags.push_back("--seed=" + std::to_string(index % 3));
            run_and_check(text, flags, tally,
                          wide ? std::nullopt : std::optional(enumerated_verdict(text)));
        }
        MESSAGE(arm.name << ": rows removed " << tally.subsume_pass_rows_removed << ", units "
                         << tally.subsume_pass_units << ", budget stops "
                         << tally.subsume_pass_budget_stops << ", sat " << tally.sat
                         << ", unsat " << tally.unsat);
        CHECK(tally.sat > 0);
        CHECK(tally.unsat > 0);
        CHECK(tally.subsume_pass_rows_removed > 0);
        if (arm.stops_at_budget) {
            CHECK(tally.subsume_pass_budget_stops > 0);
        }
    }
}

} // namespace xorcle
