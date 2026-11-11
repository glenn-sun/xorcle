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
#include <optional>
#include <vector>

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
};

struct BvePlan {
    /// @brief Whether a resolvent was the empty clause, in which case nothing
    /// else here is to be applied.
    bool found_empty_clause = false;

    /// @brief The clauses read that the pass does not leave, ascending.
    std::vector<ClauseId> removed_ids;

    /// @brief A clause a resolvent was resolved from: a clause read by its id,
    /// or a resolvent by its index in `resolvents`.
    struct Parent {
        bool is_resolvent;
        std::size_t idx;
    };

    /// @brief A resolvent: the clause if the pass left it, and else where its
    /// rows are among the clauses a later elimination removed.
    struct Resolvent {
        std::array<Parent, 2> parents;
        std::optional<Clause> clause;
        std::size_t elimination_idx = 0;
        std::size_t removed_clause_idx = 0;
    };

    /// @brief Every resolvent the pass derived, in the order derived.
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
/// @param rng Drawn from for the order and for each choice among equals.
/// @pre `active_clause_ids` are ids of `clauses`, each over `num_vars` variables
/// in representation `repr`; none is a tautology, and no two have one span.
/// @pre `1 <= config.group_cap <= 16`, `config.max_refusals_in_a_row >= 1` and
/// `0 <= config.success_rate <= 1`.
BvePlan build_bve_plan(const std::vector<Clause>& clauses, ActiveClauseIds active_clause_ids,
                       std::size_t num_vars, EquationRepr repr, const BveConfig& config, Rng& rng);

} // namespace xorcle
