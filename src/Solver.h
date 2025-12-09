#pragma once

#include <variant>
#include <queue>
#include <algorithm>
#include "Trail.h"

enum class DecisionProcedure { fixed_unit, random_unit, random_eq, vsids_unit };

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

struct VsidsComparator {
    const std::vector<double>* activity;
    VsidsComparator(const std::vector<double>& activity) : activity(&activity) {}
    bool operator()(std::size_t x, std::size_t y) const {
        return (*activity)[x] < (*activity)[y];
    }
};

using ClauseId = std::size_t;
using EquationId = std::size_t;

// class Watchlist {
// private:
//     std::vector<bit::vector<>> equations;
//     // watches :: EquationId -> std::set<ClauseId>
//     std::vector<std::set<ClauseId>> watches;
//     // unreduced_forms :: EquationId (of a reduced equation) -> std::set<EquationId>
//     std::vector<std::set<EquationId>> unreduced_forms;
//     // reduces_to :: EquationId (of any equation) -> std::set<EquationId>
//     std::vector<std::set<EquationId>> reduces_to;

//     const Trail& trail;

// public:
//     Watchlist(const Trail& trail) : trail(trail) {}

//     void add_clause(const Clause& clause, ClauseId cid) {
//         for (Equation eqn : {clause.row(clause.watched0), clause.row(clause.watched1)}) {
//             auto eid_it = std::find(equations.begin(), equations.end(), eqn);
//             if (eid_it == equations.end()) {
//                 equations.push_back(eqn);
//                 watches.push_back({cid});
//                 unreduced_forms.push_back(std::set<EquationId>());
//             } else {
//                 EquationId eid = std::distance(equations.begin(), eid_it);
//                 watches[eid].insert(cid);
//             }
//         }
//     }
// };

class Watchlist {
public:
    std::vector<std::vector<ClauseId>> by_leading_term;
    Watchlist(std::size_t num_vars) : by_leading_term(num_vars) {}
};


/// @brief Main solver class.
class Solver {
private:
    const std::size_t num_vars;
    std::deque<Clause> clauses;
    std::size_t clause_idx = 0;
    Trail trail;
    Watchlist watchlist;

    /// @brief Determine if the given clause can learn an equation.
    /// Returns std::nullopt if not possible
    /// @param clause The clause to test
    std::optional<Equation> unit_prop_one_clause(Clause& clause);

    DecisionProcedure dec_proc;

    // Only used for DecisionProcedure::{fixed_unit, random_unit, random_eq}
    std::vector<Equation> decision_basis;

    struct VsidsParams {
        std::vector<double> activity;
        std::priority_queue<std::size_t, std::vector<std::size_t>, VsidsComparator> order_heap;
        double inc_amt = 1;
        double decay = 0.95;

        VsidsParams(std::size_t n) : activity(n, 0), order_heap(VsidsComparator(activity)) {};

        void bump_activity(std::size_t v) {
            activity[v] += inc_amt;
            if (activity[v] > 1e100) {
                for (std::size_t i = 0; i < activity.size(); i++)
                    activity[i] *= 1e-100;
                inc_amt *= 1e-100;
            }

            // Update heap
            // TODO: Currently not needed because heap is completely 
            // recomputed at end of conflict analysis
        }
    };
    // Only used for DecisionProcedure::{vsids_unit}
    VsidsParams vsids_params;

public:
    Solver(const std::size_t num_vars, const DecisionProcedure dec_proc) : 
        num_vars(num_vars), 
        dec_proc(dec_proc),
        vsids_params(num_vars),
        watchlist(num_vars)
    {
        reinitialize_decision_procedure();
    };

    inline void reinitialize_decision_procedure() {
        switch (dec_proc) {
            case DecisionProcedure::fixed_unit: {
                decision_basis.clear();
                for (std::size_t i = 0; i < num_vars; i++) {
                    decision_basis.push_back(Equation::unit(num_vars + 1, i));
                }
                break;
            }
            case DecisionProcedure::random_unit: {
                decision_basis.clear();
                for (std::size_t i = 0; i < num_vars; i++) {
                    decision_basis.push_back(Equation::unit(num_vars + 1, i));
                }
            
                std::random_device rd;
                std::mt19937 g(rd());
            
                std::shuffle(decision_basis.begin(), decision_basis.end(), g);
            }
            case DecisionProcedure::random_eq: {
                decision_basis.clear();
                REFMatrix M;
                while (decision_basis.size() < num_vars) {
                    Equation eqn = Equation::random(num_vars + 1);
                    Equation eqn_reduced = M.reduce_copy(eqn);
                    if (eqn_reduced != eqn_false && eqn_reduced != eqn_true) {
                        decision_basis.push_back(eqn_reduced);
                        M.add_row(eqn_reduced);
                    }
                }
            }
            case DecisionProcedure::vsids_unit: {
                vsids_params.activity.assign(num_vars, 0);
                vsids_params.order_heap = std::priority_queue<std::size_t, std::vector<std::size_t>, VsidsComparator>(VsidsComparator(vsids_params.activity));
                for (std::size_t i = 0; i < num_vars; i++) {
                    vsids_params.order_heap.push(i);
                }
                vsids_params.inc_amt = 1;
                break;
            }
        }
    }

    std::size_t get_num_vars() const { return num_vars; }
    std::size_t get_num_clauses() const { return clauses.size(); }

    /// @brief Number of decisions made so far.
    std::size_t level() const { return trail.size() - 1; }

    /// @brief Add a clause to the solver
    void add_clause(Clause& c);

    const Equation eqn_true{Equation::zeros(num_vars + 1)};
    const Equation eqn_false{Equation::unit(num_vars + 1, num_vars)};

    /// @brief Run unit propagation to find a single learnable equation.
    /// Returns std::nullopt if no equation is learnable.
    std::optional<std::pair<Equation, std::reference_wrapper<Clause>>> unit_propagate_once();

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
    /// The DIMACS file format is extended as follows:
    ///
    /// Instead of a list of positive and negative literals, we have
    /// a list of tuples. Each tuple represents a linear equation. 
    /// Variables in the equation may be positive or negative. An even 
    /// number of negative literals expresses that the equation sums to 
    /// 1. An odd number of negative literals expresses that the equation
    /// sums to 0.
    /// 
    /// Spaces around parentheses are optional. As usual, each line
    /// is terminated by 0. Comments are lines starting with "c". The line
    /// "p cnf n m" must come before all clauses, where "n" is the number
    /// of variables and "m" is the number of clauses. For backwards 
    /// compatability with standard DIMACS format, "k" is treated 
    /// identically to "(k)" in all clauses. 
    ///
    /// Example: The following is a valid file that consists of four
    /// equivalent ways to express [x1 + x2 = 1] v [x3 = 0].
    ///
    /// c This is a comment
    /// p cnf 3 4
    /// c This is another comment
    /// (1 2) (-3) 0
    /// (-1 -2) (-3) 0
    /// c This is yet another comment
    /// ( 1 2 ) -3 0
    /// (-1 -2 ) ( -3) 0
    ///
    /// All four ways will be stored as [x1 + x2 = 0] ^ [x3 = 1].
    static Solver from_DIMACS(const std::string& filename, const DecisionProcedure dec_proc);
};

