/// @file vivify.cpp
/// @brief Vivification: narrowing kept clauses by assuming rows of their linear
/// negation on the solver's own trail.

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "algebra/equation.h"
#include "algebra/reduce_result.h"
#include "algebra/row_echelon_matrix.h"
#include "core/hint_trail.h"
#include "core/solver.h"
#include "util/index_set.h"
#include "util/random.h"

namespace xorcle {

namespace {

/// @brief The narrowest clauses a pass tries; width 2 is the implication graph's.
constexpr std::size_t vivify_min_width = 3;

} // namespace

#ifndef NDEBUG
bool Solver::debug_is_narrowing(const Clause& narrowed, const ClauseId id) const {
    assert(trail_.level() == 0 && "a narrowing is told at the root");
    const RowSpace& root_rows = trail_.span();
    RowSpace clause_rows(repr_, active_num_vars_);
    Equation reduced(repr_, active_num_vars_);
    for (const Equation& row : clause_database_.clause(id)) {
        reduced.assign_from(row);
        root_rows.reduce(reduced);
        clause_rows.add_row(reduced);
    }
    for (const Equation& row : narrowed) {
        reduced.assign_from(row);
        root_rows.reduce(reduced);
        clause_rows.reduce(reduced);
        if (!reduced.is_eq_true()) {
            return false;
        }
    }
    return true;
}
#endif

std::vector<ClauseId> Solver::vivify_proof_hints(const Clause& narrowed,
                                                 const std::size_t last_level,
                                                 const std::optional<ClauseId> falsified_id) const {
    HintTrail implied(repr_, active_num_vars_);
    implied.restart(narrowed);
    std::vector<ClauseId> hints;
    // Cite a clause as the checker replays it; one that adds nothing is left out.
    const auto cite = [&](const auto& rows, const ClauseId hint) {
        const Hint result = implied.cite(rows);
        assert(result != Hint::not_unit && "a reason on the trail is not unit under what it implies");
        if (result != Hint::known) {
            hints.push_back(hint);
        }
        return result == Hint::conflicting;
    };
    const std::size_t num_rows = trail_.rows_at_level(last_level);
    for (std::size_t trail_idx = 0; trail_idx < num_rows; ++trail_idx) {
        const std::optional<ClauseId>& reason = trail_.reasons()[trail_idx];
        if (!reason.has_value()) {
            continue; // An assumed row, which the linear negation holds.
        }
        bool conflicts = false;
        if (trail_.level_of(trail_idx) == 0) {
            // The root unit's own line, whose row is this one reduced by the units before it.
            Equation negated = trail_.row(trail_idx);
            negated.negate();
            conflicts = cite(std::span<const Equation>(&negated, 1), proof_root_unit_ids_[trail_idx]);
        } else {
            conflicts = cite(clause_database_.clause(*reason), proof_id_of_clause_[*reason]);
        }
        if (conflicts) {
            return hints;
        }
    }
    if (falsified_id.has_value() &&
        cite(clause_database_.clause(*falsified_id), proof_id_of_clause_[*falsified_id])) {
        return hints;
    }
    assert(false && "the trail does not refute the narrowed clause");
    return hints;
}

std::optional<VivifyNarrowing> Solver::vivify_one_order(const ClauseId id) {
    assert(trail_.level() == 0 && "an order starts at the root");
#ifndef NDEBUG
    const std::size_t debug_root_rows = trail_.span().rows();
#endif

    // The store may move while the trail is built on, so the rows are copied.
    std::vector<Equation> rows;
    for (const Equation& row : clause_database_.clause(id)) {
        rows.push_back(row);
    }
    const std::size_t width = rows.size();
    assert(width >= 2 && "a unit clause has nothing to narrow");

    const auto to_clause = [this](const std::vector<Equation>& kept_rows) {
        Clause clause(repr_, active_num_vars_);
        for (const Equation& row : kept_rows) {
            clause.add_row(row);
        }
        return clause;
    };

    // What assuming one more row of the linear negation found.
    enum class Step : std::uint8_t { implied, assumed, refuted, conflicting };
    std::vector<Equation> assumed; // By level less one.
    std::size_t refuted_level = 0;
    Equation reduced(repr_, active_num_vars_);
    const auto assume = [&](const Equation& row) {
        reduced.assign_from(row);
        const ReduceResult::Kind kind = trail_.span().partial_reduce(reduced).kind;
        if (kind == ReduceResult::Kind::eq_true) {
            // Implied by the rows assumed so far, so it falls out of the clause.
            return Step::implied;
        }
        if (kind == ReduceResult::Kind::eq_false) {
            refuted_level = shallowest_satisfying_level(row);
            return Step::refuted;
        }
        open_level(reduced, std::nullopt);
        ++num_vivify_rows_assumed_;
        assumed.push_back(row);
        return propagate_until_conflict() ? Step::conflicting : Step::assumed;
    };

    // A uniformly random ordered basis, drawn a row at a time.
    RowEchelonMatrix drawn(repr_, width);
    Equation coefficients = Equation::with_num_bits(repr_, width);
    std::vector<std::size_t> coefficient_bits;
    Equation last_row(repr_, active_num_vars_);
    Step step = Step::implied;
    for (std::size_t drawn_rows = 0;
         drawn_rows < width && (step == Step::implied || step == Step::assumed); ++drawn_rows) {
        while (true) {
            const IndexSet drawn_coefficients = random_nonzero_coefficients(width, vivify_rng_);
            coefficient_bits.clear();
            drawn_coefficients.for_each_set(
                [&](const std::size_t row_idx) { coefficient_bits.push_back(row_idx); });
            coefficients.assign_bits(coefficient_bits);
            if (drawn.add_row_get_row_idx(coefficients).has_value()) {
                break;
            }
        }
        last_row.set_eq_true();
        for (const std::size_t row_idx : coefficient_bits) {
            last_row.plus_assign(rows[row_idx]);
        }
        step = assume(last_row);
    }
    // With every row implied or assumed the clause itself is falsified, which
    // propagation short of a fixed point may not have found.
    const bool falsified_unfound = step == Step::implied || step == Step::assumed;
    assert((!falsified_unfound || budget_.has_value()) &&
           "an order implied the clause's every row without a conflict");
    if (falsified_unfound) {
        step = Step::conflicting;
    }
    if (step == Step::refuted && refuted_level == 0) {
        if (trail_.level() != 0) {
            backjump_to(0);
        }
        return std::nullopt;
    }

    // On a conflict the rows assumed; on a refuted row the rows assumed up to the
    // level refuting it, with that row.
    const auto rows_reached = [&] {
        std::vector<Equation> kept = assumed;
        if (step == Step::refuted) {
            kept.resize(refuted_level);
            kept.push_back(last_row);
        }
        return kept;
    };
    std::optional<VivifyNarrowing> result;
    const std::vector<Equation> kept = rows_reached();
    if (kept.size() < width) {
        Clause narrowed = to_clause(kept);
        std::vector<ClauseId> hints;
        if (proof_writer_ != nullptr) {
            hints = vivify_proof_hints(
                narrowed, step == Step::refuted ? refuted_level : trail_.level(),
                falsified_unfound ? std::optional<ClauseId>(id) : std::nullopt);
        }
        result = VivifyNarrowing{.clause = std::move(narrowed), .proof_hints = std::move(hints)};
    }
    if (trail_.level() != 0) {
        backjump_to(0);
    }
    assert(trail_.span().rows() == debug_root_rows && "an order left rows on the root trail");
    return result;
}

bool Solver::vivify_replace(const ClauseId id, VivifyNarrowing narrowing,
                            std::vector<std::pair<Equation, ClauseId>>& units) {
    Clause& narrowed = narrowing.clause;
    narrowed.reduce_rows();
    const std::size_t width = clause_database_.clause(id).rows();
    assert(debug_is_narrowing(narrowed, id) &&
           "a clause was to be replaced by one that does not imply it");
    ++stats_.vivify_narrowed;
    vivify_width_removed_ += width - narrowed.rows();
    if (narrowed.rows() == 1) {
        // Fed once the pass is over: a unit met above the root would be learned there.
        const ClauseId proof_line = write_proof_line(narrowed, narrowing.proof_hints);
        Equation unit = narrowed.row(0);
        unit.negate();
        units.emplace_back(std::move(unit), proof_line);
        return false;
    }
    return put_narrowing_in_place(id, std::move(narrowed), narrowing.proof_hints);
}

bool Solver::put_narrowing_in_place(const ClauseId id, Clause narrowed,
                                    const std::vector<ClauseId>& proof_hints) {
    assert(trail_.level() == 0 && "a clause is overwritten at the root");
    assert(narrowed.rows() >= 2 && "a unit goes through the unit path, not in a clause's place");
    equivalent_clauses_.remove(id);
    const std::optional<ClauseId> kept_id = equivalent_clauses_.find_or_enter(
        narrowed, narrowed.rows_hash(active_var_to_original_), id, clause_database_.clauses());
    if (kept_id.has_value()) {
        // A kept clause already has the narrowing's span, so the clause goes.
        ++stats_.num_equivalent_clauses;
        redirect_clause_state(id, *kept_id);
        if (proof_writer_ != nullptr) {
            proof_writer_->write_deleted({proof_id_of_clause_[id]});
        }
        deactivate_clause(id);
        return false;
    }
    if (proof_writer_ != nullptr) {
        const ClauseId proof_line = write_proof_line(narrowed, proof_hints);
        proof_writer_->write_deleted({proof_id_of_clause_[id]});
        proof_id_of_clause_[id] = proof_line;
    }
    clause_database_.replace_clause(id, std::move(narrowed));
    selector_->update_with_narrowed_clause(id);
    decisions_->update_with_narrowed_clause(id);
    if (implication_graph_.has_value()) {
        implication_graph_->clause_was_rewritten(id);
    }
    return true;
}

std::vector<ClauseId> Solver::vivify_candidates() {
    const IndexSet is_locked = trail_.reason_clause_ids(clause_database_.num_clauses());

    std::vector<ClauseId> ids;
    for (const ClauseId id : clause_database_.active_clause_ids()) {
        if (clause_database_.clause(id).rows() < vivify_min_width) {
            continue;
        }
        if (clause_database_.record(id).vivified) {
            continue;
        }
        if (!is_locked.test(id)) {
            ids.push_back(id);
        }
    }
    return ids;
}

bool Solver::vivify_is_due() const {
    if (!vivify_.has_value()) {
        return false;
    }
    const std::size_t conflicts_since = stats_.num_conflicts - conflicts_at_last_vivify_pass_;
    if ((vivify_has_run_ && conflicts_since == 0) || conflicts_since < vivify_->cadence) {
        return false;
    }
    const std::span<const std::size_t> num_by_width =
        clause_database_.num_active_clauses_by_width();
    return !std::all_of(num_by_width.begin() + static_cast<std::ptrdiff_t>(std::min(
                                                    vivify_min_width, num_by_width.size())),
                        num_by_width.end(), [](const std::size_t num) { return num == 0; });
}

void Solver::vivify_pass() {
    assert(trail_.level() == 0 && "a vivification pass runs at the root");
    vivify_has_run_ = true;
    phase_timer_.switch_to(SolverPhase::vivification);
    const std::size_t rows = search_rows();
    const double budget =
        vivify_->effort * static_cast<double>(rows - search_rows_at_last_vivify_pass_);
    conflicts_at_last_vivify_pass_ = stats_.num_conflicts;
    search_rows_at_last_vivify_pass_ = rows;

#ifndef NDEBUG
    clause_database_.debug_assert_widths_counted();
#endif
    std::vector<ClauseId> ids = vivify_candidates();
    if (ids.empty()) {
        return;
    }

    // The assumptions and what they imply are not the search's.
    const PropagationCounters search_counters = std::exchange(propagation_counters_, {});

    const std::size_t assumed_before = num_vivify_rows_assumed_;
    const auto out_of_budget = [&] {
        const std::size_t spent = num_vivify_rows_assumed_ - assumed_before +
                                  propagation_counters_.num_propagations;
        return static_cast<double>(spent) >= budget;
    };

    std::vector<std::pair<Equation, ClauseId>> units;
    shuffle_uniformly(ids, vivify_rng_);
    for (const ClauseId id : ids) {
        // A clause whose coin fails stays a candidate.
        const double draw_below =
            std::pow(2.0, 3.0 - static_cast<double>(clause_database_.clause(id).rows()));
        if (draw_below < 1.0 && uniform_below_one(vivify_rng_) >= draw_below) {
            continue;
        }
        if (out_of_budget()) {
            break;
        }
        // Each order on the clause as the last one left it; a unit or an eviction ends it.
        bool in_place = true;
        for (std::size_t round = 0; round < vivify_->rounds && in_place &&
                                    clause_database_.clause(id).rows() >= vivify_min_width;
             ++round) {
            in_vivify_try_ = true;
            std::optional<VivifyNarrowing> narrowed = vivify_one_order(id);
            in_vivify_try_ = false;
            if (narrowed.has_value()) {
                in_place = vivify_replace(id, std::move(*narrowed), units);
            }
        }
        if (in_place) {
            clause_database_.record(id).vivified = true;
        }
    }

    assert(debug_equivalent_clause_table_is_exact() &&
           "the equivalent clause table is out of step with the active clauses");
    assert(debug_no_two_clauses_share_a_span() && "two active clauses have one span");

    // An earlier unit of this pass can already imply a later one.
    feed_units(units, RowSpace(repr_, active_num_vars_));

    propagation_counters_ = search_counters;
}

} // namespace xorcle
