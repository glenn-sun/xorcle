/// @file test_fuzz.cpp
/// @brief Whole runs on random instances, each answer checked independently.

#include <doctest/doctest.h>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
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
#include "io/proof_writer.h"
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
    RunStats counters; ///< Summed over the runs, for the coverage checks.
};

/// @brief Whether an assignment over the original variables satisfies a clause,
/// given by its rows over them.
bool clause_holds(const std::vector<ProofRow>& rows, const IndexSet& assignment) {
    for (const ProofRow& row : rows) {
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
    const std::vector<std::vector<ProofRow>> rows = clause_rows(parsed);

    std::ostringstream proof;
    Solution solution;
    {
        ProofWriter writer(proof, num_vars);
        const std::unique_ptr<Solver> solver =
            make_solver(*options, num_vars, parsed.used_vars, &writer);
        for (Clause& clause : parsed.clauses) {
            solver->add_starting_clause(std::move(clause));
        }
        solution = solver->solve();

        const RunStats counters = solver->stats();
        tally.counters.num_conflicts += counters.num_conflicts;
        tally.counters.num_compactions += counters.num_compactions;
        tally.counters.num_deletion_passes += counters.num_deletion_passes;
        tally.counters.num_subsumed_clauses += counters.num_subsumed_clauses;
        tally.counters.num_equivalent_clauses += counters.num_equivalent_clauses;
        tally.counters.num_restarts += counters.num_restarts;
        tally.counters.num_undone_decisions += counters.num_undone_decisions;
        tally.counters.implication_graph_units_fed += counters.implication_graph_units_fed;
        tally.counters.subsume_pass_passes += counters.subsume_pass_passes;
    }

    REQUIRE(solution.result == expected.value_or(solution.result));
    if (solution.result == SolveResult::sat) {
        ++tally.sat;
        REQUIRE(solution.assignment.has_value());
        REQUIRE(solution.assignment->size() == num_vars);
        for (const std::vector<ProofRow>& clause : rows) {
            CHECK(clause_holds(clause, *solution.assignment));
        }
        return solution.result;
    }

    ++tally.unsat;
    LrupChecker checker(options->equation_repr, num_vars, rows);
    CHECK(checker.check(proof.str()) == "");
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
        std::size_t RunStats::*exercised = nullptr;
    };
    // Deletion passes and the graph are due every few conflicts.
    const std::vector<std::string> often = {"--deletion-cadence-coefficient=0.1",
                                            "--deletion-cadence-offset=0",
                                            "--deletion-wait=0",
                                            "--implication-graph-cadence=4"};
    std::vector<std::string> often_without_graph(often.begin(), often.end() - 1);
    often_without_graph.emplace_back("--implication-graph=false");
    const std::vector<Configuration> configurations = {
        {"defaults", {}},
        {"defaults, often", often, &RunStats::num_deletion_passes},
        {"no graph, often", often_without_graph, &RunStats::num_deletion_passes},
        {"cyclic, complete", {"--selection=cyclic", "--propagate-miss-rate=0"}},
        {"cyclic, miss rate 0.05",
         {"--selection=cyclic", "--propagate-window=4"}},
        {"sparse", {"--equation-repr=sparse"}},
        {"cyclic, sparse",
         {"--selection=cyclic", "--equation-repr=sparse", "--propagate-miss-rate=0.5",
          "--propagate-window=1", "--csids-intersection-depth=8"},
         &RunStats::num_undone_decisions},
        {"luby",
         {"--restart=luby", "--restart-luby-unit=1"},
         &RunStats::num_restarts},
        {"span plus-minus", {"--csids-span=plus_minus"}},
        {"span plus-minus, random side, no phase saving",
         {"--csids-span=plus_minus", "--phase=random", "--phase-saving=none"}},
        {"phase saving", {"--phase-saving=both", "--phase=random"}},
        {"phase saving, span plus-minus",
         {"--phase-saving=both", "--phase=random", "--csids-span=plus_minus"}},
        {"phase saving, target reset often",
         {"--phase-saving=both", "--phase=random", "--phase-saving-target-reset=10"}},
        {"phase saving, top clause alone",
         {"--phase-saving=both", "--phase=random", "--csids-intersection-depth=1"}},
        {"phase saving, luby restarts, no recent trail",
         {"--phase-saving=target-only", "--phase=random", "--restart=luby"}},
        {"phase saving, no target, sparse",
         {"--phase-saving=recent-only", "--phase=random", "--selection=cyclic",
          "--equation-repr=sparse"}},
        {"phase saving, target reset every conflict",
         {"--phase-saving=both", "--phase=random", "--phase-saving-target-reset=1"}},
        {"phase rotation, short steps", {"--phase-saving=rotate", "--phase-rotate-interval=2"}},
        {"phase rotation, span plus-minus",
         {"--phase-saving=rotate", "--phase-rotate-interval=3", "--csids-span=plus_minus"}},
        {"phase saving, recent trail alone, falsify side", {"--phase-saving=recent-only"}},
        {"stable and focused modes, short phases", {"--stabilize=true", "--stabilize-interval=2"}},
        {"stable and focused modes under rotation",
         {"--stabilize=true", "--stabilize-interval=3", "--phase-saving=rotate",
          "--phase-rotate-interval=2"}},
        {"stable and focused modes, luby restarts in stable mode",
         {"--stabilize=true", "--stabilize-interval=2", "--restart=luby", "--restart-luby-unit=2"}},
        {"stable and focused modes, both with LBD restarts, focused mode rotating",
         {"--stabilize=true", "--stabilize-interval=2", "--restart=lbd",
          "--restart-lbd-min-conflicts=3", "--phase-saving=both",
          "--focused-phase-saving=rotate", "--phase-rotate-interval=2"}},
        {"stable and focused modes, trail blocking, slow average 1000",
         {"--stabilize=true", "--stabilize-interval=1", "--focused-restart-lbd-slow=1000",
          "--focused-restart-lbd-min-conflicts=3", "--focused-restart-lbd-trail-ratio=1.4"}},
    };

    // Equivalent clauses are evicted at input and at a substitution; at a subsumption
    // only on instances larger than these, which a roundtrip test covers.
    std::size_t num_evictions = 0;
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
            // Instances this small have too few width-2 clauses for a run the default
            // threshold leaves standing.
            if (std::ranges::find(flags, "--implication-graph=false") == flags.end()) {
                flags.emplace_back("--implication-graph-min-new-clauses=0");
            }
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
        CHECK((tally.counters.implication_graph_units_fed > 0) != graph_off);
        if (configuration.exercised != nullptr) {
            CHECK(tally.counters.*configuration.exercised > 0);
        }
        num_evictions += tally.counters.num_equivalent_clauses;
    }
    CHECK(num_evictions > 0);
}

// Its answers are held to brute force, its models to the input and its proofs,
// each narrowing a line, to the checker.
TEST_CASE("fuzz: vivification leaves the verdict, the models and the proofs right") {
    struct Arm {
        std::string name;
        std::vector<std::string> flags;
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
        {"forced restarts, one round, a conflict apart",
         {"--vivify=true", "--vivify-cadence=0", "--vivify-rounds=1", "--vivify-effort=1000"}},
        {"the default effort",
         {"--vivify=true", "--vivify-cadence=5"}},
    };
    for (const Arm& arm : arms) {
        CAPTURE(arm.name);
        TestRandom random(20261013);
        RunStats total;
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
            const std::vector<std::vector<ProofRow>> rows = clause_rows(parsed);
            std::ostringstream proof;
            ProofWriter writer(proof, parsed.num_vars);
            const std::unique_ptr<Solver> solver =
                make_solver(*options, parsed.num_vars, parsed.used_vars, &writer);
            for (Clause& clause : parsed.clauses) {
                solver->add_starting_clause(std::move(clause));
            }
            const Solution solution = solver->solve();
            if (index % 5 != 4) {
                REQUIRE(solution.result == enumerated_verdict(text));
            }
            if (solution.result == SolveResult::sat) {
                ++num_sat;
                REQUIRE(solution.assignment.has_value());
                for (const std::vector<ProofRow>& clause : rows) {
                    CHECK(clause_holds(clause, *solution.assignment));
                }
            } else {
                ++num_unsat;
                LrupChecker checker(options->equation_repr, parsed.num_vars, rows);
                CHECK(checker.check(proof.str()) == "");
            }
            const RunStats counters = solver->stats();
            total.vivify_narrowed += counters.vivify_narrowed;
            total.vivify_units += counters.vivify_units;
            total.num_equivalent_clauses += counters.num_equivalent_clauses;
        }
        MESSAGE(arm.name << ": narrowed " << total.vivify_narrowed << ", units "
                         << total.vivify_units << ", equivalent clauses "
                         << total.num_equivalent_clauses << ", sat " << num_sat << ", unsat "
                         << num_unsat);
        CHECK(num_sat > 0);
        CHECK(num_unsat > 0);
        CHECK(total.vivify_narrowed > 0);
    }
}

// The subsumption pass deletes and strengthens clauses with a proof line each, so
// its answers are held to brute force, their models and their proofs; the wide
// instances only to the models and proofs.
TEST_CASE("fuzz: the subsumption pass leaves every answer right and checked") {
    struct Arm {
        std::string name;
        std::vector<std::string> flags;
    };
    const std::vector<Arm> arms = {
        {"every root visit, nearly unbudgeted",
         {"--inproc-subsume-strengthen=true", "--inproc-subsume-strengthen-cadence=0", "--inproc-subsume-strengthen-effort=1000000"}},
        {"a conflict apart, nearly unbudgeted",
         {"--inproc-subsume-strengthen=true", "--inproc-subsume-strengthen-cadence=1", "--inproc-subsume-strengthen-effort=1000000"}},
        {"no renumbering, a conflict apart, nearly unbudgeted",
         {"--inproc-subsume-strengthen=true", "--inproc-subsume-strengthen-cadence=1", "--compact=false",
          "--inproc-subsume-strengthen-effort=1000000"}},
        {"cyclic with a miss rate, sparse",
         {"--inproc-subsume-strengthen=true", "--inproc-subsume-strengthen-cadence=2", "--selection=cyclic",
          "--equation-repr=sparse", "--propagate-miss-rate=0.5", "--propagate-window=1"}},
        {"a conflict apart, the default effort",
         {"--inproc-subsume-strengthen=true", "--inproc-subsume-strengthen-cadence=1"}},
        {"every root visit, effort 1",
         {"--inproc-subsume-strengthen=true", "--inproc-subsume-strengthen-cadence=0", "--inproc-subsume-strengthen-effort=1"}},
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
        MESSAGE(arm.name << ": passes " << tally.counters.subsume_pass_passes
                         << ", subsumed or strengthened " << tally.counters.num_subsumed_clauses
                         << ", sat " << tally.sat
                         << ", unsat " << tally.unsat);
        CHECK(tally.sat > 0);
        CHECK(tally.unsat > 0);
        CHECK(tally.counters.subsume_pass_passes > 0);
    }
}

} // namespace xorcle
