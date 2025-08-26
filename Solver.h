#pragma once

#include <vector>
#include <string>
#include <optional>
#include <functional>
#include <random>
#include <algorithm>
#include "REFMatrix.h"
#include "Heap.h"
#include <atomic>
#include <variant>

#define FIXED_UNIT 0
#define RANDOM_UNIT 1
#define RANDOM_EQ 2
// #define VSIDS_UNIT 3

#define DECISION_PROCEDURE FIXED_UNIT

/// @brief Method to print equations.
///
/// - standard: No constants on LHS.
///
/// - equals_0: LHS = 0, possible constants on LHS.
///
/// - equals_1: LHS = 1, possible constants on LHS.
enum class EquationPrintMode { standard, equals_0, equals_1 };

/// @brief Method to print clauses.
///
/// - as_clause: OR of original equations.
///
/// - as_rowspace: AND of negated equations (as in representation).
enum class ClausePrintMode { as_clause, as_rowspace };

struct PrintMode {
    EquationPrintMode equation_mode = EquationPrintMode::standard;
    ClausePrintMode clause_mode = ClausePrintMode::as_clause;
};

/// @brief An affine equation over GF(2).
///
/// Represented as a bit::vector whose last component is the constant 
/// term. In other words, if there are 3 variables, the equation 
/// x1 + x3 = 1 is represented as [1, 0, 1, 1]. Note that the false
/// equation 1 = 0 has a representation in this form: when there are
/// 3 variables, it is [0, 0, 0, 1].
class Equation : public bit::vector<> {
public:
    Equation() {}
    Equation(const bit::vector<>& v) : bit::vector<>(v) {}
    std::string to_string(PrintMode mode = PrintMode()) const;
};
    
/// @brief A vector of coefficients over GF(2).
/// 
/// Used when representing equations as linear combinations of other
/// equations. The length of this vector is the dimension of the current
/// subspace, not the number of variables + 1. 
class Coefficients : public bit::vector<> {
public:
    Coefficients() {}
    Coefficients(const bit::vector<>& v) : bit::vector<>(v) {}
};


/// @brief A subspace of the space of linear equations over GF(2),
/// represented as a basis in row-echelon form. Each column corresponds
/// to a variable in the original order.
class RowSpace : public REFMatrix {
public:
    /// @brief Get the unique solution to the system of equations represented by this matrix.
    ///
    /// Requires: The matrix is full-rank and does not contain the false equation.
    bit::vector<> get_unique_solution() const;
};

/// @brief A clause in its annihilator representation, as the conjunction
/// of linear equations whose solutions are where the clause is false.
///
/// Example: The clause [x1 + x2 = 1] v [x3 = 0] is represented as
/// [x1 + x2 = 0] ^ [x3 = 1], i.e. [[1, 1, 0, 0], [0, 0, 1, 1]].
class Clause : public RowSpace {
public:
    // Index of the first watched equation
    std::size_t watched0 = 0;

    // Index of the second watched equation
    std::size_t watched1 = 1;

    // When a clause is true, set this to the index of the true equation
    // to avoid re-computing it
    std::size_t true_witness = 0;

    std::string to_string(PrintMode mode = PrintMode()) const;

    Clause() {}
    Clause(const RowSpace& rs) : RowSpace(rs) {}
};

/// @brief A snapshot of all entries at level <= some k in the trail.
class TrailSnapshot {
public:
    // Map from trail insertion order to the row in representation
    std::vector<std::size_t> order_map;

    // Map from row in representation to trail insertion order
    std::vector<std::size_t> reverse_order_map;

    // Space of known true equations
    RowSpace span;

    // Map from trail insertion order to reason
    std::vector<std::optional<std::reference_wrapper<Clause>>> reasons;

    /// @brief Learn a new equation. Requires that eqn + 1 belongs to the span of reason.
    /// eqn will be reduced by the current trail span before learning.
    /// @param eqn The equation to learn
    /// @param reason The reason for learning eqn, or std::nullopt if eqn is a decision
    void learn(const Equation& eqn, std::optional<std::reference_wrapper<Clause>> reason);
};

/// @brief The trail of known true equations is stored in back(). Each intermediate snapshot
/// is a point in time that is can be backjumped to. trail[k] stores all true equations at
/// level <= k, and all snapshots except the first contain a decision as the first equation.
class Trail : public std::vector<TrailSnapshot> {
public:
    Trail() : std::vector<TrailSnapshot>() {
        push_back(TrailSnapshot());
    }

    /// @brief Learn a new equation. Requires that eqn + 1 belongs to the span of reason.
    /// eqn will be reduced by the current trail span before learning. Creates a new snapshot
    /// if eqn is a decision.
    /// @param eqn The equation to learn
    /// @param reason The reason for learning eqn, or std::nullopt if eqn is a decision
    void learn(const Equation& eqn, std::optional<std::reference_wrapper<Clause>> reason) {
        if (!reason.has_value()) {
            TrailSnapshot new_snapshot = back();
            push_back(new_snapshot);
        }
        back().learn(eqn, reason);
    }

    /// @brief The space of known true equations
    const RowSpace& span() const { 
        return back().span; 
    }

    /// @brief The map from trail insertion order to the row in representation
    const std::vector<std::size_t>& order_map() const {
        return back().order_map;
    }

    /// @brief The map from row in representation to trail insertion order
    const std::vector<std::size_t>& reverse_order_map() const {
        return back().reverse_order_map;
    }

    /// @brief The map from trail insertion order to reason
    const std::vector<std::optional<std::reference_wrapper<Clause>>>& reasons() const {
        return back().reasons;
    }
};

/// @brief A subspace of a trail span, represented as a basis in 
/// row-echelon form. Each column corresponds to an equation in the trail,
/// in the reverse trail appending order (i.e. the first column corresponds
/// to the last equation in the trail, i.e. often false).
class TrailBasisSpace : public REFMatrix {
private:
    const Trail& trail;

public:
    TrailBasisSpace(const Trail& trail) : REFMatrix(), trail(trail) {}

    /// @brief Construct a TrailBasisSpace from a RowSpace w.r.t. a Trail
    ///
    /// In particular, if the trail contains m linearly independent equations, 
    /// and the row space contains k equations, then this will have k equations 
    /// in m variables, where the i-th "variable" is the coefficient of the 
    /// trail.order_map[m-i-1]-th row in trail.span().
    ///
    /// For example, if the trail is [[x1 + x2 = 1], [x3 = 0]] with [x3 = 0] inserted
    /// before [x1 + x2 = 1], and the row space is simply [x3 = 0], then 
    /// in the trail basis this is [0, 1].
    ///
    /// Requires: rs belongs to trail.span()
    static TrailBasisSpace from_RowSpace(const RowSpace& rs, const Trail& trail);

    /// @brief Convert this TrailBasisSpace back to the original RowSpace
    RowSpace to_RowSpace() const;

    /// @brief Determine the maximum number of trail snapshots for which the
    /// second row starts is zero. The clause is asserting if jumpable_level() > 0.
    std::size_t jumpable_levels() const;
};

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
    std::deque<Clause> clauses;
    Trail trail;

    #if DECISION_PROCEDURE == FIXED_UNIT || DECISION_PROCEDURE == RANDOM_UNIT || DECISION_PROCEDURE == RANDOM_EQ
        std::vector<Equation> decision_basis;

    // #elif DECISION_PROCEDURE == VSIDS_UNIT
    //     std::vector<double> activity;
    //     Heap<std::size_t> order_heap;
    //     std::vector<bool> in_heap;
    //     double inc_amt = 1;
    //     double decay = 0.95;
    #endif
public:
    Solver(const std::size_t num_vars) : num_vars(num_vars)
    // #if DECISION_PROCEDURE == VSIDS_UNIT
    //     , activity(num_vars, 0.0), order_heap(activity), in_heap(num_vars, true)
    // #endif
    {
        reinitialize_decision_procedure();
    };

    inline void reinitialize_decision_procedure() {
        
        #if DECISION_PROCEDURE == FIXED_UNIT 
            decision_basis.clear();
            for (std::size_t i = 0; i < num_vars; i++) {
                decision_basis.push_back(Equation::unit(num_vars + 1, i));
            }
        #elif DECISION_PROCEDURE == RANDOM_UNIT
            decision_basis.clear();
            for (std::size_t i = 0; i < num_vars; i++) {
                decision_basis.push_back(Equation::unit(num_vars + 1, i));
            }
        
            std::random_device rd;
            std::mt19937 g(rd());
        
            std::shuffle(decision_basis.begin(), decision_basis.end(), g);
        #elif DECISION_PROCEDURE == RANDOM_EQ
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
            std::cout << "done" << std::endl;
        // #elif DECISION_PROCEDURE == VSIDS_UNIT
        //     activity = std::vector<double>(num_vars, 0.0);
        //     Heap<std::size_t> order_heap(activity);
        //     in_heap = std::vector<bool>(num_vars, true);
        #endif
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
    std::pair<Clause, std::size_t>  find_asserting_clause(const Trail& trail) const;

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
    static Solver from_DIMACS(const std::string& filename);
};

