#pragma once

#include <variant>
#include <algorithm>
#include <memory>
#include "Trail.h"
#include "DecisionProcedure.h"
#include "ClauseSelector.h"
#include "ExactWatchlist.h"

namespace ResultCase {
    struct Timeout {};
    struct Unsat {};
    struct Sat {
        bit::vector<> values;
        explicit Sat(bit::vector<> v) : values(std::move(v)) {}
    };

    inline bool is_timeout(const std::variant<Timeout, Unsat, Sat>& result) {
        return std::holds_alternative<Timeout>(result);
    }
    
    inline bool is_unsat(const std::variant<Timeout, Unsat, Sat>& result) {
        return std::holds_alternative<Unsat>(result);
    }
    
    inline bool is_sat(const std::variant<Timeout, Unsat, Sat>& result) {
        return std::holds_alternative<Sat>(result);
    }

    inline const bit::vector<>& unwrap_sat(const std::variant<Timeout, Unsat, Sat>& result) {
        if (const Sat* sat = std::get_if<Sat>(&result)) {
            return sat->values;
        }
        throw std::runtime_error("Expected Sat result");
    }
}
using Result = std::variant<ResultCase::Timeout, ResultCase::Unsat, ResultCase::Sat>;

/// @brief Main solver class.
class Solver {
private:
    const std::size_t num_vars;
    Trail trail;
    std::shared_ptr<ClauseSelector> clause_sel;
    std::shared_ptr<DecisionProcedure> dec_proc;

    /// @brief Determine if the given clause can learn an equation.
    /// Returns std::nullopt if not possible
    /// @param clause The clause to test
    std::optional<Equation> unit_prop_one_clause(ClauseId id);

    Equation working_eqn;
    Equation working_eqn2;

public:
    Solver(const std::size_t num_vars, std::shared_ptr<ClauseSelector> clause_sel_in, std::shared_ptr<DecisionProcedure> dec_proc) : 
        num_vars(num_vars), 
        trail(num_vars),
        clause_sel(std::move(clause_sel_in)),
        dec_proc(std::move(dec_proc)),
        working_eqn(bit::vector<>::zeros(num_vars + 1)),
        working_eqn2(bit::vector<>::zeros(num_vars + 1)) {};

    Solver(const std::size_t num_vars, std::shared_ptr<DecisionProcedure> dec_proc) :
        Solver(num_vars, std::make_shared<Cyclic>(num_vars), std::move(dec_proc)) {}

    std::size_t get_num_vars() const { return num_vars; }
    std::size_t get_num_clauses() const { 
        return clause_sel->get_num_clauses();
    }

    /// @brief Number of decisions made so far.
    std::size_t level() const { return trail.size() - 1; }

    // /// @brief Add a clause to the solver
    // void add_clause(Clause& clause) {
    //     if (clause.rows() == 0) { 
    //         return;
    //     }

    //     clause_sel->add_clause(clause);
    // }

    const std::vector<Clause>& get_clauses() const {
        return clause_sel->get_clauses();
    }

    Clause& get_clause(ClauseId id) { 
        return clause_sel->get_clause(id); 
    }

    const Equation eqn_true{Equation::zeros(num_vars + 1)};
    const Equation eqn_false{Equation::unit(num_vars + 1, num_vars)};

    /// @brief Run unit propagation to find a single learnable equation.
    /// Returns std::nullopt if no equation is learnable.
    std::optional<std::pair<Equation, ClauseId>> unit_propagate_once();

    /// @brief Find a clause that is asserting with respect to the trail.
    /// A clause is asserting if it creates a unit propgation from trail prior
    /// to the last decision. Also returns the number of levels to jump back.
    ///
    /// Requires: trail is complete (ends with contradiction)
    std::pair<Clause, std::size_t>  find_asserting_clause(const Trail& trail);

    /// @brief Determine if the set of clauses is satisfiable. If it is,
    /// return a satisfying assignment. Otherwise, return std::nullopt.
    Result solve(std::atomic<bool>& stop_flag);

    void print_clauses(const PrintMode mode = PrintMode{}) const;
    void print_trail(const EquationPrintMode eqn_mode = EquationPrintMode::standard) const;

    /// @brief Create a Solver from a DIMACS file.
    /// @param filename Path to a DIMACS file.
    /// 
    /// The DIMACS file format is extended as in [Danner, Kreuzer].
    ///
    /// In the standard DIMACS format, each literal is either an integer
    /// or a negated integer. In the extended format, the basic expressions
    /// are XOR expressions with infix "+". They may be negated by affixing
    /// a leading "-".
    ///
    /// Example: The following file expresses a clause [x1 + x2 = 0] v [x3 = 1].
    ///
    /// c This is a comment
    /// p xnf 3 4
    /// -1+2 3 0
    ///
    /// In our clause encoding, note that this will be stored as [x1 + x2 = 1] ^ [x3 = 0].
    static Solver from_DIMACS(const std::string& filename, const DecisionProcedureName dpn, const ClauseSelectorName csn);

    // Statistics
    std::size_t num_attempt_props = 0;
    std::size_t num_watch_updates = 0;
    std::size_t num_propagations = 0;
    std::size_t num_conflicts = 0;

};
