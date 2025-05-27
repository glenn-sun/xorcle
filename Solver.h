#pragma once

#include <vector>
#include <string>
#include <optional>
#include <functional>
#include "bit/vector.h"
#include "bit/matrix.h"

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
using Equation = bit::vector<>;

/// @brief A subspace of the space of linear equations over GF(2),
/// represented as a basis in row-echelon form. Each column corresponds
/// to a variable in the original order.
class RowSpace : public bit::matrix<> {};

/// @brief A subspace of a trail span, represented as a basis in 
/// row-echelon form. Each column corresponds to an equation in the trail,
/// in the trail appending order.
class TrailBasisSpace : public bit::matrix<> {
public:
    /// @brief Construct a TrailBasisSpace from a RowSpace w.r.t. a Trail
    ///
    /// In particular, if the solver has n variables, the trail contains m 
    /// linearly independent equations, and the row space contains k equations,
    /// then this will have k equations in m variables, the i-th "variable"
    /// being the coefficient of the trail.order_map[i]-th row in trail.span().
    ///
    /// For example, if the trail is [[x1 + x2 = 1], [x3 = 0]] with [x3 = 0] inserted
    /// before [x1 + x2 = 1], and the row space is simply [x3 = 0], then 
    /// in the trail basis this is [1, 0].
    static TrailBasisSpace from_RowSpace(const RowSpace& rs, const Trail& trail);
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

    /// @brief Determine if this clause is asserting with repsect to the given
    /// trail. A clause is asserting if it falsified by (contained in) trail.span(), 
    /// and its negation leads to a conflict by unit propagation, and it unit propagates 
    /// an equation from trail.prev_span(). 
    ///
    /// Requires: The clause must be falsified by (contained in) trail.span() and
    /// its negation must lead to a conflict by unit propagation.
    ///
    /// @param trail The trail to check against.
    bool is_asserting(const Trail& trail) const;
};

/// @brief A snapshot of all entries at level <= some k in the trail.
class TrailSnapshot {
public:
    // Map from trail insertion order to the row in representation
    std::vector<std::size_t> order_map;

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

    /// @brief The map from trail insertion order to reason
    const std::vector<std::optional<std::reference_wrapper<Clause>>>& reasons() const {
        return back().reasons;
    }
};

/// @brief Main solver class.
class Solver {
private:
    const std::size_t num_vars;
    std::vector<Clause> clauses;
    Trail trail;

public:
    Solver(const std::size_t num_vars) : num_vars(num_vars) {};

    /// @brief Number of decisions made so far.
    std::size_t level() const { return trail.size() - 1; }

    /// @brief Add a clause to the solver
    void add_clause(Clause& c);

    const Equation eqn_true{num_vars + 1};
    const Equation eqn_false{Equation::unit(num_vars + 1, num_vars)};

    /// @brief Run unit propagation to find a single learnable equation.
    /// Returns std::nullopt if no equation is learnable.
    std::optional<std::pair<Equation, std::reference_wrapper<Clause>>> unit_propagate_once();

    /// @brief Determine if the set of clauses is satisfiable. If it is,
    /// return a satisfying assignment. Otherwise, return std::nullopt.
    std::optional<bit::vector<>> solve();

    void print_clauses(const PrintMode mode = PrintMode{}) const;
    void print_trail(const PrintMode mode = PrintMode{}) const;

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

