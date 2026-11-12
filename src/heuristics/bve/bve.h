#pragma once

/// @file bve.h
/// @brief Bounded variable elimination by projection onto spans of the clauses.
///
/// A direction is an equation without its constant, and a clause contains the
/// directions of the sums of its equations. A flip set is a set of variables: it
/// changes the directions that have an odd number of them, and touches the
/// clauses that contain a direction it changes. Eliminating by a flip set
/// resolves the clauses it touches, after which an assignment satisfies the
/// formula exactly when it or it with the set flipped satisfied the old one.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "algebra/equation.h"
#include "algebra/equation_repr.h"
#include "algebra/row_echelon_matrix.h"
#include "core/clause_id.h"
#include "heuristics/bve/elimination.h"
#include "util/index_set.h"
#include "util/random.h"

namespace xorcle {

struct BveConfig {
    /// @brief The most rows a group may hold.
    std::size_t group_cap;

    /// @brief Refusals in a row after which a group is given up.
    std::size_t max_refusals_in_a_row;

    /// @brief The least fraction of the clauses present at a pass's start that the
    /// pass must remove for another to follow.
    double success_rate;

    /// @brief The most conflicts one solve of a gate search may take.
    std::size_t gate_max_conflicts;

    /// @brief Whether a one-sided gate's root equations are added as unit clauses.
    bool root_equations;
};

/// @brief What one BVE pass did.
struct BveCounters {
    std::size_t num_eliminated = 0;
    std::size_t clauses_before = 0;
    std::size_t clauses_after = 0;
    std::size_t width_before = 0; ///< Of the clauses before, summed.
    std::size_t width_after = 0; ///< Of the clauses after, summed.
    std::size_t num_unit_resolvents = 0; ///< Among the clauses left.
    std::size_t num_resolvents_of_width_at_most_2 = 0; ///< Among the clauses left, units included.
    std::size_t num_eliminated_by_gate = 0; ///< Of `num_eliminated`, those whose resolution found a gate.
    /// @brief Equations one-sided gates make false at the root, added as unit clauses.
    std::size_t num_root_equations = 0;
    double gate_time_s = 0.0; ///< Of the gate searches.
};

struct BvePlan {
    /// @brief Whether a resolvent was the empty clause, in which case nothing
    /// else here is to be applied.
    bool found_empty_clause = false;

    /// @brief The clauses read that the pass does not leave, ascending.
    std::vector<ClauseId> removed_ids;

    /// @brief A clause a line cites: a clause read by its id, a resolvent by its
    /// index in `resolvents`, or a lemma of the same resolvent by its index in `lemmas`.
    struct Parent {
        enum class Kind : std::uint8_t { clause, resolvent, lemma };
        Kind kind;
        std::size_t idx;
    };

    /// @brief A clause derived on the way to a root equation, and the clauses its line cites.
    struct Lemma {
        Clause clause;
        std::vector<Parent> reasons;
    };

    /// @brief A resolvent, or a root equation of a one-sided gate as a unit clause: the
    /// clause if the pass left it, and else where its rows are among the clauses a
    /// later elimination removed.
    struct Resolvent {
        std::array<Parent, 2> parents; ///< Of a resolvent.
        /// @brief Of a root equation, its line's lemmas and the clauses it cites, in replay order.
        std::vector<Lemma> lemmas;
        std::vector<Parent> reasons;
        std::optional<Clause> clause;
        std::size_t elimination_idx = 0;
        std::size_t removed_clause_idx = 0;
    };

    /// @brief Every resolvent and root equation the pass derived, in the order derived.
    std::vector<Resolvent> resolvents;

    /// @brief In the order eliminated.
    std::vector<Elimination> eliminations;

    /// @brief The variables the pass took out of every clause, over `num_vars + 1` columns.
    IndexSet dropped_col_idxs;

    BveCounters counters;
};

/// @brief One pass: eliminate by flip sets chosen by projection, accepting an
/// elimination when its resolvents have no more rows in total than the clauses it removes.
///
/// The clauses are read in a uniformly random order, which splits the span of
/// their directions into a core subspace and groups; the hyperplanes containing
/// the core subspace and every other group are tried per group, fewest
/// clauses touched first, until `config.max_refusals_in_a_row` are refused in a
/// row. See arXiv:2605.15002 for projection onto a span.
///
/// @param with_proof Whether each root equation's plan carries the lines that derive it.
/// @param rng Drawn from for the order and for each choice among equals.
/// @pre `active_clause_ids` are ids of `clauses`, each over `num_vars` variables
/// in representation `repr`; none is a tautology, and no two have one span.
/// @pre `1 <= config.group_cap <= 16`, `config.max_refusals_in_a_row >= 1`,
/// `0 <= config.success_rate <= 1` and `config.gate_max_conflicts >= 1`.
BvePlan build_bve_plan(const std::vector<Clause>& clauses, ActiveClauseIds active_clause_ids,
                       std::size_t num_vars, EquationRepr repr, const BveConfig& config,
                       bool with_proof, Rng& rng);

/// @brief A basis of the equations the spans of @p negations share, each of which a
/// flip set changes, given @p unchanged, the equations of the first span it does not
/// change; empty if it changes none of them.
///
/// When the clauses' rests are unsatisfiable together, each equation of the basis
/// is false at the root: the gate is one-sided.
/// @pre @p negations is not empty, and @p unchanged is a hyperplane of its first span.
std::vector<Equation> changed_shared_equations(const std::vector<Clause>& negations,
                                               const Clause& unchanged);

} // namespace xorcle
