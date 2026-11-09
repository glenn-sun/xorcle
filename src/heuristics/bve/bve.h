#pragma once

/// @file bve.h
/// @brief Bounded variable elimination by flip sets, chosen by projection onto spans of the clauses.
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
    bool enabled = false;

    /// @brief A choice touching n clauses is tried with chance 2^(try_from - n), at most 1.
    double try_from = 10.0;

    /// @brief The most tail rows a tail group may hold.
    std::size_t tail_group_cap = 10;

    /// @brief The least fraction of the clauses present at a reading's start that the
    /// reading must remove for another to follow.
    double success_rate = 0.01;

    bool operator==(const BveConfig&) const = default;
};

/// @brief What one BVE pass did.
struct BveCounters {
    std::size_t num_eliminated = 0;
    std::size_t num_eliminated_by_gate = 0;
    std::size_t num_eliminated_in_one_clause = 0; ///< Of `num_eliminated`, with no clause to resolve against.
    std::size_t num_units_substituted = 0; ///< Units among the resolvents, not in `num_eliminated`.
    std::size_t num_clauses_tested_alone = 0; ///< For a flip set that touches the clause alone.
    std::size_t clauses_before = 0;
    std::size_t clauses_after = 0;
    std::size_t equation_bits = 0; ///< Of the formula the pass started on.
    std::size_t steps_to_keep_rows = 0; ///< The steps of choosing the kept rows, not in `num_steps`.
    /// @brief One per word of an equation or a sum of kept rows read or written, counted as
    /// dense words whatever the representation, 16 per equation, one per four truth-table
    /// words, one per pair of clauses looked at and one per clause or held vector listed.
    std::size_t num_steps = 0;
    std::size_t num_unit_resolvents = 0; ///< Among the clauses left.
    std::size_t num_resolvents_of_width_at_most_2 = 0; ///< Among the clauses left, units included.
    double time_s = 0.0;
    std::size_t num_readings = 0;
    std::size_t num_readings_eliminating = 0;
    std::size_t num_tail_groups = 0; ///< Over every reading.
    std::size_t num_tail_rows = 0; ///< Over every reading.
    std::array<std::size_t, 17> tail_groups_of_size{}; ///< Over every reading, by tail rows.
    /// @brief Clauses with rows of their own that went into the core span, past the tail group cap.
    std::size_t num_held_in_core = 0;
    std::size_t num_held_in_core_alone = 0; ///< Of those, with more rows of their own than the cap.
    std::size_t width_held_in_core = 0; ///< Their widths summed.
    /// @brief Over every reading, the share of the order from its end to the first clause of a tail
    /// group, in parts per million.
    std::size_t walk_depth_ppm = 0;
    std::size_t max_walk_depth_ppm = 0;
    /// @brief Over every reading, the clauses using each tail row and each core row, summed.
    std::size_t users_of_tail_rows = 0;
    std::size_t users_of_core_rows = 0;
    std::size_t num_core_rows = 0;
    std::size_t num_choices_tried = 0;
    std::size_t num_not_drawn = 0; ///< Of those, not drawn to be tried.
    /// @brief Of those, not resolved as the same rows of the same clauses were refused before.
    std::size_t num_choices_refused_before = 0;
    std::size_t num_not_accepted = 0; ///< Of those, resolved and not accepted.
    std::size_t num_choices_accepted = 0;
};

/// @brief What the BVE passes of one solve did between them.
struct BveTotals {
    std::size_t num_passes = 0;
    std::size_t num_eliminated = 0;
    std::size_t num_eliminated_by_gate = 0;
    std::size_t num_eliminated_in_one_clause = 0;
    std::size_t num_units_substituted = 0;
    std::size_t num_clauses_tested_alone = 0;
    std::size_t clauses_before = 0; ///< Of the first pass.
    std::size_t clauses_after = 0; ///< Of the last pass.
    std::size_t equation_bits = 0; ///< Of the formula the first pass started on.
    std::size_t num_steps = 0;
    std::size_t steps_to_keep_rows = 0;
    std::size_t num_resolvents_of_width_at_most_2 = 0;
    double time_s = 0.0;
    std::size_t num_readings = 0;
    std::size_t num_readings_eliminating = 0;
    std::size_t num_tail_groups = 0;
    std::size_t num_tail_rows = 0;
    std::size_t num_held_in_core = 0;
    std::size_t num_choices_tried = 0;
    std::size_t num_not_drawn = 0;
    std::size_t num_not_accepted = 0;
    std::size_t num_choices_accepted = 0;

    void add_pass(const BveCounters& pass) {
        if (num_passes++ == 0) {
            clauses_before = pass.clauses_before;
            equation_bits = pass.equation_bits;
        }
        num_eliminated += pass.num_eliminated;
        num_eliminated_by_gate += pass.num_eliminated_by_gate;
        num_eliminated_in_one_clause += pass.num_eliminated_in_one_clause;
        num_units_substituted += pass.num_units_substituted;
        num_clauses_tested_alone += pass.num_clauses_tested_alone;
        clauses_after = pass.clauses_after;
        num_steps += pass.num_steps;
        steps_to_keep_rows += pass.steps_to_keep_rows;
        num_resolvents_of_width_at_most_2 += pass.num_resolvents_of_width_at_most_2;
        time_s += pass.time_s;
        num_readings += pass.num_readings;
        num_readings_eliminating += pass.num_readings_eliminating;
        num_tail_groups += pass.num_tail_groups;
        num_tail_rows += pass.num_tail_rows;
        num_held_in_core += pass.num_held_in_core;
        num_choices_tried += pass.num_choices_tried;
        num_not_drawn += pass.num_not_drawn;
        num_not_accepted += pass.num_not_accepted;
        num_choices_accepted += pass.num_choices_accepted;
    }
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

/// @brief Eliminate by flip sets chosen by projection, accepting an elimination
/// when its resolvents have no more rows in total than the clauses it removes.
///
/// Readings of the clauses in uniformly random orders, each splitting the span
/// of their directions into a core span and tail groups; the hyperplanes
/// containing the core span and every other tail group are tried per tail
/// group, fewest rows touched first, and a clause some flip set touches alone
/// is removed; until a reading removes fewer than `config.success_rate` of the
/// clauses present at its start, or none. See arXiv:2605.15002 for projection
/// onto a span.
///
/// @param rng Drawn from for each choice among equals.
/// @pre `active_clause_ids` are ids of `clauses`, each over `num_vars` variables
/// in representation `repr`.
/// @pre `config.try_from >= 0`, `1 <= config.tail_group_cap <= 16` and `0 <= config.success_rate <= 1`.
BvePlan build_bve_plan(const std::vector<Clause>& clauses, ActiveClauseIds active_clause_ids,
                       std::size_t num_vars, EquationRepr repr, const BveConfig& config, Rng& rng);

} // namespace xorcle
