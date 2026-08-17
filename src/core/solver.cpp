#include "core/solver.h"

#include <algorithm>
#include <cassert>
#include <numeric>
#include <optional>
#include <utility>
#include <vector>

#include "algebra/reduce_result.h"
#include "core/analysis.h"
#include "heuristics/compaction/original_space.h"

namespace xorcle {

Solver::Solver(Algebra backend, std::size_t num_vars, std::unique_ptr<ClauseSelector> selector,
               std::unique_ptr<DecisionProcedure> decisions,
               std::unique_ptr<RestartPolicy> restarts, LrupLogger* proof_logger,
               const CompactionConfig& compaction)
    : backend_(backend),
      original_num_vars_(num_vars),
      active_num_vars_(num_vars),
      active_var_to_original_(num_vars),
      compaction_(compaction),
      trail_(backend, num_vars),
      selector_(std::move(selector)),
      decisions_(std::move(decisions)),
      restarts_(std::move(restarts)),
      propagation_(backend, num_vars),
      proof_logger_(proof_logger),
      conflict_check_eqn_(backend, num_vars),
      false_equation_(Equation::eq_false(backend, num_vars)) {
    assert(selector_ != nullptr);
    assert(decisions_ != nullptr);
    assert(restarts_ != nullptr);
    assert((proof_logger_ == nullptr || !compaction_.enabled) &&
           "proof emission with compaction on is not supported; see "
           "require_compaction_disabled_for_proof()");
    assert((proof_logger_ == nullptr || proof_logger_->num_vars() == original_num_vars_) &&
           "the proof logger's variable space must be the instance's");
    // The active space starts as the original one, so the map is the identity.
    std::iota(active_var_to_original_.begin(), active_var_to_original_.end(), std::size_t{0});
}

void Solver::add_starting_clause(Clause clause) {
    assert(compaction_history_.empty() && "input clauses must be added before solving");
    assert(clause.cols() == active_num_vars_ + 1);

    const ClauseId new_clause_id = selector_->num_clauses();
    // Before the selector stores it: with no pending learned activity recorded,
    // CSIDS gives every input clause activity zero.
    decisions_->update_with_new_clause(clause, new_clause_id);
    selector_->add_starting_clause(std::move(clause));
    next_proof_clause_id_ = selector_->num_clauses();
}

bool Solver::propagate_until_conflict() {
    assert(active_var_to_original_.size() == active_num_vars_);
    assert(false_equation_.size() == active_num_vars_ + 1);
    assert(conflict_check_eqn_.size() == active_num_vars_ + 1);

    while (true) {
        const std::optional<ClauseSelection> selection = selector_->next_with_hint();
        if (!selection.has_value()) {
            return false;
        }
        assert(selection->id < selector_->num_clauses() && "selector returned an unknown clause");
        assert(selector_->is_clause_active(selection->id) &&
               "selector returned an inactive clause");
        ++counters_.num_attempt_props;

        Equation* propagated_eqn = propagation_.unit_propagate_one_clause(
            selection->id, selector_->clause(selection->id), trail_, selection->zero_watch_hint,
            counters_.num_watch_updates, *selector_);
        if (propagated_eqn == nullptr) {
            continue;
        }

        ++counters_.num_propagations;
        assert(propagated_eqn->size() == active_num_vars_ + 1);

        // Re-normalize: the engine reduced the clause row it propagated from,
        // not the negation it returns, so the result can still hold span rows.
        (void)trail_.span().partial_reduce(*propagated_eqn);
        trail_.learn(*propagated_eqn, selection->id);
        // The selector is notified even on the propagation that conflicts.
        selector_->update_with_propagation(*propagated_eqn, trail_);

        // Learn first, *then* check for a conflict. Conflict analysis's identity
        // holds only because the most recent trail row is 1 at that moment, and
        // this ordering is what makes that true; testing before learning would
        // leave the conflicting equation outside the trail and silently
        // invalidate every resolution step.
        conflict_check_eqn_.assign_from(false_equation_);
        if (trail_.span().partial_reduce(conflict_check_eqn_).kind ==
            ReduceResult::Kind::eq_true) {
            return true;
        }
    }
}

void Solver::handle_non_root_conflict() {
    std::vector<ClauseId> reason_ids;
    AssertingClause learned =
        find_asserting_clause(trail_, selector_->clauses(), *decisions_,
                              proof_logger_ != nullptr ? &reason_ids : nullptr);

    // The id the clause is *about* to take, read before it is added.
    const ClauseId new_clause_id = selector_->num_clauses();

    // Records the activity the new clause will enter at, and decays. Must precede
    // the `update_with_new_clause()` below, which spends that recorded activity;
    // swapping the two changes every learned clause's activity.
    decisions_->update_with_asserting_clause(learned.clause);

    assert((learned.clause.rows() != 1 || learned.asserting_level == 0) &&
           "a unit asserting clause must assert at the root");
    const std::size_t level_before_backjump = trail_.level();
    // Consumes a conflict from the restart budget, so it must run exactly once
    // per non-root conflict whether or not a restart fires.
    const bool restart_now = restarts_->should_restart_after_conflict();

    // How many levels the *solver* discards, which is what the selector is told.
    // Without a restart that is the complement of the asserting level; with one
    // the solver drops every non-root level regardless of where the clause
    // asserts. Passing the asserting level here, or its complement
    // unconditionally, is wrong in exactly the restart case.
    std::size_t removed_levels = level_before_backjump - learned.asserting_level;
    if (restart_now) {
        removed_levels = level_before_backjump;
        trail_.resize(1);
        restarts_->note_restart();
        ++counters_.num_restarts;
    } else {
        trail_.resize(learned.asserting_level + 1);
    }

    decisions_->update_with_new_clause(learned.clause, new_clause_id);

    if (proof_logger_ != nullptr) {
        // Analysis collects reasons in derivation order, conflicting clause
        // first; the proof format wants replay order. Exactly one reversal, here.
        std::reverse(reason_ids.begin(), reason_ids.end());
        proof_logger_->log_learned_clause(new_clause_id, learned.clause, reason_ids);
    }

    selector_->add_asserting_clause(std::move(learned.clause), trail_, removed_levels);
    next_proof_clause_id_ = std::max(next_proof_clause_id_, selector_->num_clauses());
}

void Solver::write_root_unsat_proof() {
    if (proof_logger_ == nullptr) {
        return;
    }
    next_proof_clause_id_ = std::max(next_proof_clause_id_, selector_->num_clauses());

    // Trail order is already replay order here, so nothing is reversed.
    std::vector<ClauseId> conflict_reason_ids;
    conflict_reason_ids.reserve(trail_.reasons().size());
    for (const std::optional<ClauseId>& reason : trail_.reasons()) {
        if (reason.has_value()) {
            conflict_reason_ids.push_back(*reason);
        }
    }
    proof_logger_->log_empty_clause(next_proof_clause_id_++, conflict_reason_ids);
}

void Solver::make_decision_step() {
    Equation decision_eqn = decisions_->make_decision(trail_.span(), selector_->clauses());
    [[maybe_unused]] const ReduceResult reduced = trail_.span().partial_reduce(decision_eqn);
    assert(decision_eqn.size() == active_num_vars_ + 1);
    // A decision must be neither 0 (already implied) nor 1 (already refuted).
    assert(reduced.kind == ReduceResult::Kind::leading &&
           "the decision procedure returned an equation the trail already decides");

    trail_.learn(decision_eqn, std::nullopt);
    selector_->update_with_decision(decision_eqn, trail_);
    selector_->update_with_decision_clause(decisions_->decision_clause_hint());
}

bool Solver::maybe_compact_before_decision() {
    const CompactionInputs inputs{
        .trail = trail_,
        .clauses = selector_->clauses(),
        .clause_is_active = selector_->active_flags(),
        .active_var_to_original = active_var_to_original_,
        .active_num_vars = active_num_vars_,
        .has_compacted_once = has_compacted_once_,
        .num_conflicts = counters_.num_conflicts,
        .conflicts_at_last_compaction = conflicts_at_last_compaction_,
        .compaction_conflict_interval = compaction_.conflict_interval,
    };
    if (!should_compact_before_decision(inputs)) {
        return false;
    }

    const CompactionTransform transform = build_compaction_transform(trail_, active_num_vars_);
    if (transform.new_num_vars() == active_num_vars_) {
        // Nothing to eliminate: a plan would rewrite every clause into itself.
        return false;
    }

    apply_compaction(build_compaction_plan(inputs, transform));
    return true;
}

void Solver::apply_compaction(CompactionPlan plan) {
    const std::size_t num_clauses = selector_->num_clauses();
    assert(plan.rewritten_clauses.size() == num_clauses);
    assert(plan.clause_is_active.size() == num_clauses);

    for (ClauseId clause_id = 0; clause_id < num_clauses; ++clause_id) {
        selector_->clause(clause_id) = std::move(plan.rewritten_clauses[clause_id]);
        selector_->set_clause_active(clause_id, plan.clause_is_active[clause_id] != 0);
    }

    // Composed, not replaced: the new map runs from the compacted space to the
    // original one through the space being retired.
    const std::size_t new_num_vars = plan.transform.new_num_vars();
    std::vector<std::size_t> compacted_var_to_original(new_num_vars);
    for (std::size_t active_var = 0; active_var < new_num_vars; ++active_var) {
        compacted_var_to_original[active_var] =
            active_var_to_original_[plan.transform.to_old(active_var)];
    }

    active_num_vars_ = new_num_vars;
    active_var_to_original_ = std::move(compacted_var_to_original);
    compaction_history_.push_back(std::move(plan.history_entry));

    propagation_.reconfigure(active_num_vars_);
    false_equation_ = Equation::eq_false(backend_, active_num_vars_);
    conflict_check_eqn_.resize(active_num_vars_);
    conflict_check_eqn_.clear();

    // The root trail is thrown away on purpose; see the warning on this method.
    trail_ = Trail(backend_, active_num_vars_);

    selector_->update_with_compaction(plan.transform, trail_);
    decisions_->update_with_compaction(plan.transform, trail_.span(), selector_->clauses(),
                                       selector_->active_flags());
    has_compacted_once_ = true;
    conflicts_at_last_compaction_ = counters_.num_conflicts;
}

Solution Solver::solve() {
    next_proof_clause_id_ = std::max(next_proof_clause_id_, selector_->num_clauses());

    while (true) {
        if (propagate_until_conflict()) {
            ++counters_.num_conflicts;
            if (level() == 0) {
                // Nothing to backjump to: the instance is unsatisfiable.
                write_root_unsat_proof();
                return Solution{SolveResult::unsat, std::nullopt};
            }
            handle_non_root_conflict();
            continue;
        }

        // Quiescent at full rank over the variable columns: every variable is
        // determined and the trail is a total assignment.
        if (trail_.span().rows() == active_num_vars_) {
            const Bitmap active_assignment = trail_.span().unique_solution();
            Bitmap original_assignment = reconstruct_original_assignment(
                active_assignment, compaction_history_, active_var_to_original_,
                original_num_vars_);
            return Solution{SolveResult::sat, std::move(original_assignment)};
        }

        if (compaction_.enabled && maybe_compact_before_decision()) {
            // The active space changed; re-propagate rather than deciding
            // against state that no longer exists.
            continue;
        }

        make_decision_step();
    }
}

} // namespace xorcle
