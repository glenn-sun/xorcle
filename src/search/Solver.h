#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "PrintMode.h"
#include "algebra/Equation.h"
#include "algebra/RowSpace.h"
#include "bit/vector.h"
#include "heuristics/ClauseSelector.h"
#include "heuristics/DecisionProcedure.h"
#include "search/PropagationEngine.h"
#include "search/Trail.h"

namespace search {
namespace ResultCase {
    /// @brief Tag for timeout results.
    struct Timeout {};

    /// @brief Tag for unsatisfiable results.
    struct Unsat {};

    /// @brief Tag for satisfiable results with assignment payload.
    struct Sat {
        bit::vector<> values; ///< Assignment values in solver order.

        /// @brief Construct with an assignment vector (moved).
        explicit Sat(bit::vector<> v) : values(std::move(v)) {}
    };

    /// @brief Return true if the result is a timeout.
    bool is_timeout(const std::variant<Timeout, Unsat, Sat>& result);

    /// @brief Return true if the result is unsatisfiable.
    bool is_unsat(const std::variant<Timeout, Unsat, Sat>& result);

    /// @brief Return true if the result is satisfiable.
    bool is_sat(const std::variant<Timeout, Unsat, Sat>& result);

    /// @brief Return the assignment from a SAT result or throw.
    /// @pre result holds ResultCase::Sat.
    const bit::vector<>& unwrap_sat(const std::variant<Timeout, Unsat, Sat>& result);
}

/// @brief Solver result variant.
using Result = std::variant<ResultCase::Timeout, ResultCase::Unsat, ResultCase::Sat>;

/// @brief Main solver class implementing propagation, decision, and learning.
class Solver {
private:
    const std::size_t num_vars; ///< Number of variables in the problem.
    Trail trail; ///< Trail of learned equations (decision levels).
    std::shared_ptr<heuristics::ClauseSelector> clause_sel; ///< Clause selector strategy.
    std::shared_ptr<heuristics::DecisionProcedure> dec_proc; ///< Decision procedure strategy.
    PropagationEngine propagation; ///< Unit propagation engine.

    algebra::Equation working_eqn; ///< Reusable buffer for conflict checks.

public:
    /// @brief Construct a solver from variable count, clause selector, and decision procedure.
    Solver(std::size_t num_vars,
           std::shared_ptr<heuristics::ClauseSelector> clause_sel_in,
           std::shared_ptr<heuristics::DecisionProcedure> dec_proc);

    /// @brief Construct a solver with a default cyclic clause selector.
    Solver(std::size_t num_vars,
           std::shared_ptr<heuristics::DecisionProcedure> dec_proc);

    /// @brief Return the variable count for this solver instance.
    std::size_t get_num_vars() const;

    /// @brief Return the number of clauses currently stored.
    std::size_t get_num_clauses() const;

    /// @brief Return current decision statistics from the decision procedure.
    heuristics::DecisionProcedure::DecisionStats decision_stats() const;

    /// @brief Number of decision levels (excluding level 0).
    std::size_t level() const;

    /// @brief Return the current clause list.
    const std::vector<algebra::Clause>& get_clauses() const;

    /// @brief Return a mutable clause reference by id.
    algebra::Clause& get_clause(heuristics::ClauseId id);

    /// @brief Constant true equation (all zeros) for conflict checks.
    const algebra::Equation eqn_true{algebra::Equation::zeros(num_vars + 1)};

    /// @brief Constant false equation (1 = 0) for conflict checks.
    const algebra::Equation eqn_false{algebra::Equation::unit(num_vars + 1, num_vars)};

    /// @brief Find an asserting clause for the current conflict.
    /// @pre trail ends with a contradiction equation.
    std::pair<algebra::Clause, std::size_t> find_asserting_clause(const Trail& trail);

    /// @brief Determine if the set of clauses is satisfiable.
    /// @param stop_flag External stop signal; set to true to stop early.
    Result solve(std::atomic<bool>& stop_flag);

    /// @brief Print all clauses using the given print mode.
    void print_clauses(PrintMode mode = PrintMode{}) const;

    /// @brief Print the current trail using the given equation print mode.
    void print_trail(EquationPrintMode eqn_mode = EquationPrintMode::standard) const;

    /// @brief Create a Solver from a DIMACS file.
    /// @param filename Path to a DIMACS file.
    /// @param dpn Decision procedure to use.
    /// @param csn Clause selector to use.
    /// @note The DIMACS format is extended as in Danner/Kreuzer to support XOR literals.
    static Solver from_DIMACS(const std::string& filename,
                              heuristics::DecisionProcedureName dpn,
                              heuristics::ClauseSelectorName csn);

    /// @brief Total number of attempted propagations.
    std::size_t num_attempt_props = 0;

    /// @brief Total number of watch updates.
    std::size_t num_watch_updates = 0;

    /// @brief Total number of successful propagations.
    std::size_t num_propagations = 0;

    /// @brief Total number of conflicts encountered.
    std::size_t num_conflicts = 0;
};
} // namespace search
