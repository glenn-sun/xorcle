/// @file root_visit.cpp
/// @brief The visit to the root that serves every inprocessing technique, the forced
/// restart that brings the search there, and the replay after it.

#include <cassert>
#include <cstddef>
#include <optional>

#include "algebra/reduce_result.h"
#include "core/solver.h"

namespace xorcle {

ClauseId Solver::Replay::kept_for(ClauseId id) const {
    for (auto found = kept_in_place_of.find(id); found != kept_in_place_of.end();
         found = kept_in_place_of.find(id)) {
        id = found->second;
    }
    return id;
}

const Equation* Solver::implied_on_trail(const ClauseId id) {
    if (clause_is_known_satisfied(clause_database_.record(id))) {
        return nullptr;
    }
    const Clause& clause = clause_database_.clause(id);
    const RowSpace& span = trail_.span();
    // Partially reduced rows lead on columns no trail row leads on, so two that lead
    // on different columns are independent modulo the span.
    bool have_first = false;
    std::size_t first_bit = 0;
    for (const Equation& row : clause) {
        Equation& reduced = have_first ? replay_reduced_eqn_ : replay_implied_eqn_;
        reduced.assign_from(row);
        ReduceResult result = span.partial_reduce(reduced);
        if (have_first && result.is_leading() && result.bit == first_bit) {
            reduced.plus_assign(replay_implied_eqn_);
            result = span.partial_reduce(reduced);
        }
        if (result.kind == ReduceResult::Kind::eq_true) {
            continue;
        }
        if (result.kind == ReduceResult::Kind::eq_false || have_first) {
            return nullptr; // An equation of the clause holds, or two are open.
        }
        have_first = true;
        first_bit = result.bit;
    }
    if (have_first) {
        replay_implied_eqn_.negate();
    } else {
        replay_implied_eqn_.set_eq_false();
    }
    return &replay_implied_eqn_;
}

Solver::PropagationOutcome Solver::learn_implied(const ClauseId id, const Equation& implied_eqn) {
    assert(!budget_.has_value() && "a budgeted run may date the equation below the top level");
    ++propagation_counters_.num_propagations;
    const std::size_t trail_idx = trail_.learn(implied_eqn, id, trail_.level());
    assert(debug_reason_implies(id, trail_idx) &&
           "a replayed equation does not follow from its reason");
    selector_->update_with_new_equation(trail_.row(trail_idx), trail_);
    if (implied_eqn.is_eq_false()) {
        conflict_clause_id_ = id;
        conflict_level_ = trail_.level();
        return PropagationOutcome::conflicting;
    }
    return PropagationOutcome::propagated;
}

Solver::VisitStep Solver::visit_root_step() {
    // Every technique below runs on a substituted root.
    if (trail_.span().rows() != 0) {
        substitute_root_units();
        return VisitStep::again;
    }
    if (bve_is_due_) {
        return run_bve() ? VisitStep::again : VisitStep::unsat;
    }
    if (inproc_subsume_strengthen_is_due()) {
        run_inproc_subsume_strengthen();
        return VisitStep::again;
    }
    if (vivify_is_due()) {
        vivify_pass();
        return VisitStep::again;
    }
    if (implication_graph_is_due()) {
        run_implication_graph();
        return VisitStep::again;
    }
    return VisitStep::done;
}

bool Solver::forced_restart_is_due() {
    // The graph first, since its test records a skipped run.
    const bool graph_is_due = implication_graph_is_due();
    return graph_is_due || inproc_subsume_strengthen_is_due() || vivify_is_due();
}

void Solver::force_restart() {
    phase_timer_.switch_to(SolverPhase::replay);
    const std::size_t num_levels = trail_.level();
    assert(num_levels > 0 && "a forced restart from the root");
    replay_.decisions.save_decisions(trail_, level_stamp_, num_levels);
    replay_.decision_clause_ids = decision_clause_of_level_;
    replay_.reason_ids.clear();
    replay_.reason_ends.clear();
    for (std::size_t level = 1; level <= num_levels; ++level) {
        for (std::size_t trail_idx = trail_.rows_at_level(level - 1) + 1;
             trail_idx < trail_.rows_at_level(level); ++trail_idx) {
            const std::optional<ClauseId>& reason = trail_.reasons()[trail_idx];
            assert(reason.has_value() && "a propagated row has no reason");
            replay_.reason_ids.push_back(*reason);
        }
        replay_.reason_ends.push_back(replay_.reason_ids.size());
    }
    replay_.kept_in_place_of.clear();
    replay_.next_level_idx = 0;
    replay_.under_way = true;

    ++stats_.num_forced_restarts;
    // The backjump can queue clauses, so the loop propagates the root before the visit.
    backjump_to(0);
}

void Solver::note_clause_kept_in_place_of(const ClauseId deleted_id, const ClauseId kept_id) {
    if (replay_.under_way) {
        replay_.kept_in_place_of[deleted_id] = kept_id;
    }
}

void Solver::end_replay() {
    if (!replay_.under_way) {
        return;
    }
    replay_.under_way = false;
    replay_.kept_in_place_of.clear();
}

Solver::DecisionStep Solver::replay_decision_step() {
    phase_timer_.switch_to(SolverPhase::replay);
    while (replay_.next_level_idx < replay_.decision_clause_ids.size()) {
        const std::size_t level_idx = replay_.next_level_idx++;
        Equation& decision_eqn = replay_.decisions.rows[level_idx];
        const ReduceResult::Kind kind = trail_.span().partial_reduce(decision_eqn).kind;
        if (kind == ReduceResult::Kind::eq_true || kind == ReduceResult::Kind::eq_false) {
            continue;
        }
        // The hint only says where the cyclic selector's next sweep starts, so a
        // decision clause the visit deleted is simply none.
        std::optional<ClauseId> decision_clause = replay_.decision_clause_ids[level_idx];
        if (decision_clause.has_value()) {
            decision_clause = replay_.kept_for(*decision_clause);
            if (!clause_database_.is_clause_active(*decision_clause)) {
                decision_clause.reset();
            }
        }
        open_level(decision_eqn, decision_clause);
        decisions_->update_with_replayed_decision();

        // The level's reasons in their old order, each the clause kept in its place;
        // one deleted with none, satisfied or not unit has nothing to propagate. Without
        // a budget the equation goes on the trail with no watch moved, as when another
        // clause implies it first, since the selector's sweep of the decision may not
        // have reached the clause yet.
        const std::size_t begin = level_idx == 0 ? 0 : replay_.reason_ends[level_idx - 1];
        for (std::size_t i = begin; i < replay_.reason_ends[level_idx]; ++i) {
            const ClauseId id = replay_.kept_for(replay_.reason_ids[i]);
            if (!clause_database_.is_clause_active(id)) {
                continue;
            }
            const Equation* implied_eqn = implied_on_trail(id);
            if (implied_eqn == nullptr) {
                continue;
            }
            const PropagationOutcome outcome =
                budget_.has_value() ? propagate_from_clause(id) : learn_implied(id, *implied_eqn);
            if (outcome == PropagationOutcome::conflicting) {
                return DecisionStep::conflicting;
            }
        }
        return DecisionStep::decided;
    }
    end_replay();
    return make_decision_step();
}

} // namespace xorcle
