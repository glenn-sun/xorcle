#include "core/solver.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <numeric>
#include <optional>
#include <utility>
#include <vector>

#include "algebra/reduce_result.h"
#include "core/analysis.h"
#include "heuristics/compaction/original_variables.h"

namespace xorcle {

Solver::Solver(EquationRepr repr, std::size_t num_vars, std::unique_ptr<ClauseSelector> selector,
               std::unique_ptr<DecisionProcedure> decisions,
               std::unique_ptr<RestartPolicy> restarts, RawProofWriter* proof_writer,
               const CompactionConfig& compaction, const ProgressConfig& progress, bool minimize,
               const DeletionConfig& deletion)
    : repr_(repr),
      original_num_vars_(num_vars),
      active_num_vars_(num_vars),
      active_var_to_original_(num_vars),
      compaction_(compaction),
      deletion_(deletion),
      trail_(repr, num_vars),
      selector_(std::move(selector)),
      decisions_(std::move(decisions)),
      restarts_(std::move(restarts)),
      propagator_(repr, num_vars),
      proof_writer_(proof_writer),
      conflict_check_eqn_(repr, num_vars),
      false_equation_(Equation::eq_false(repr, num_vars)),
      progress_(progress),
      minimize_(minimize) {
    assert(selector_ != nullptr && "the clause selector must not be null");
    assert(decisions_ != nullptr && "the decision procedure must not be null");
    assert(restarts_ != nullptr && "the restart policy must not be null; use NoRestarts");
    assert((proof_writer_ == nullptr || proof_writer_->num_vars() == original_num_vars_) &&
           "the proof writer's variable numbering must be the instance's");
    assert((!deletion_.enabled || deletion_.key != DeletionKey::activity ||
            deletion_.activity != nullptr) &&
           "the activity key has no activity source");
    std::iota(active_var_to_original_.begin(), active_var_to_original_.end(), std::size_t{0});
}

void Solver::add_starting_clause(Clause clause) {
    assert(!has_solved_ && "input clauses must be added before solving");
    assert(compaction_history_.empty() && "input clauses must be added before any compaction");
    assert(clause.cols() == active_num_vars_ + 1 &&
           "a starting clause must be over the solver's variable count");
    assert(clause.repr() == repr_ &&
           "a starting clause must use the solver's representation");

    const ClauseId new_clause_id = selector_->num_clauses();
    decisions_->update_with_new_clause(clause, new_clause_id);
    selector_->add_starting_clause(std::move(clause));
    next_proof_clause_id_ = selector_->num_clauses();
}

bool Solver::propagate_until_conflict() {
    assert(active_var_to_original_.size() == active_num_vars_);
    assert(false_equation_.num_bits() == active_num_vars_ + 1);
    assert(conflict_check_eqn_.num_bits() == active_num_vars_ + 1);

    while (true) {
        const std::optional<ClauseSelection> selection = selector_->next_with_hint();
        if (!selection.has_value()) {
            return false;
        }
        assert(selection->id < selector_->num_clauses() && "selector returned an unknown clause");
        assert(selector_->is_clause_active(selection->id) &&
               "selector returned an inactive clause");
        ++counters_.num_attempt_props;

        Equation* propagated_eqn = propagator_.unit_propagate_one_clause(
            selection->id, selector_->clause(selection->id), trail_, selection->trigger_hint,
            counters_.num_watch_updates, *selector_);
        if (propagated_eqn == nullptr) {
            continue;
        }

        ++counters_.num_propagations;
        assert(propagated_eqn->num_bits() == active_num_vars_ + 1);

        // Re-normalize: the propagator reduced the clause row it propagated from,
        // not the negation it returns, so the result can still hold span rows.
        (void)trail_.span().partial_reduce(*propagated_eqn);
        trail_.learn(*propagated_eqn, selection->id);
        // The selector is notified even on the propagation that conflicts.
        selector_->update_with_propagation(*propagated_eqn, trail_);

        conflict_check_eqn_.assign_from(false_equation_);
        if (trail_.span().partial_reduce(conflict_check_eqn_).kind ==
            ReduceResult::Kind::eq_true) {
            return true;
        }
    }
}

void Solver::handle_non_root_conflict() {
    AssertingClause learned = find_asserting_clause(trail_, selector_->clauses(), *decisions_,
                                                    minimize_, deletion_.enabled);
    if (minimize_) {
        counters_.minimize_dim_before += learned.dim_before_minimize;
        counters_.minimize_dim_after += learned.clause.rows();
        counters_.num_minimized_clauses +=
            learned.clause.rows() < learned.dim_before_minimize ? 1 : 0;
    }
    if (deletion_.enabled) {
        // Before the proof path reverses `reason_ids`, which `reason_lbds` parallels.
        for (std::size_t i = 0; i < learned.reason_ids.size(); ++i) {
            note_reason_use(learned.reason_ids[i], learned.reason_lbds[i]);
        }
    }

    // The id the clause is *about* to take, read before it is added.
    const ClauseId new_clause_id = selector_->num_clauses();

    // Records the activity the new clause will enter at, and decays. Must precede
    // the `update_with_new_clause()` below, which spends that recorded activity.
    decisions_->update_with_asserting_clause(learned.clause);

    assert((learned.clause.rows() != 1 || learned.asserting_level == 0) &&
           "a unit asserting clause must assert at the root");
    const std::size_t level_before_backjump = trail_.level();
    // Consumes a conflict from the restart budget, so it must run exactly once
    // per non-root conflict whether or not a restart fires.
    const bool restart_now = restarts_->should_restart_after_conflict(
        ConflictSummary{.lbd = learned.lbd, .trail_size = trail_.span().rows()});

    // How many levels the *solver* discards. Without a restart that is the
    // complement of the asserting level; with one it is everything below the
    // levels trail reuse keeps.
    std::size_t removed_levels = level_before_backjump - learned.asserting_level;
    backjump_to(learned.asserting_level);
    if (restart_now) {
        // Asked on the backjumped trail: it is not conflicting, and a restart
        // can only discard further than the conflict already has.
        std::size_t kept_levels = 0;
        if (restarts_->reuses_trail()) {
            kept_levels = decisions_->reusable_levels(trail_.span(), selector_->clauses(),
                                                      selector_->active_flags(),
                                                      decision_clause_of_level_);
            assert(kept_levels <= learned.asserting_level && "reuse kept a discarded level");
        }
        counters_.num_reused_levels += kept_levels;
        removed_levels = level_before_backjump - kept_levels;
        backjump_to(kept_levels);
        restarts_->note_restart();
        ++counters_.num_restarts;
    }

    decisions_->update_with_new_clause(learned.clause, new_clause_id);

    if (proof_writer_ != nullptr) {
        // Derivation order to replay order. Exactly one reversal, here.
        std::reverse(learned.reason_ids.begin(), learned.reason_ids.end());
        proof_writer_->write_learned(new_clause_id, learned.clause, active_var_to_original_,
                                     learned.reason_ids);
    }

    const AssertingClauseInfo learned_info{.lbd = learned.lbd, .removed_levels = removed_levels};
    selector_->add_asserting_clause(std::move(learned.clause), trail_, learned_info);
    next_proof_clause_id_ = std::max(next_proof_clause_id_, selector_->num_clauses());
}

void Solver::write_root_unsat_proof() {
    if (proof_writer_ == nullptr) {
        return;
    }
    next_proof_clause_id_ = std::max(next_proof_clause_id_, selector_->num_clauses());
    proof_writer_->write_empty(next_proof_clause_id_++,
                               find_root_conflict_reasons(trail_, selector_->clauses()));
}

void Solver::write_compaction_units() {
    assert(proof_writer_ != nullptr && "no proof to record a compaction in");

    // Trail order is a derivation order: `add_row()` never back-substitutes, so
    // each stored row is spanned by the reasons of the rows up to it.
    const TrailSnapshot& root = trail_[0];
    std::vector<RawUnit> units;
    units.reserve(root.span().rows());
    for (std::size_t t = 0; t < root.span().rows(); ++t) {
        const std::optional<ClauseId>& reason = root.reasons()[t];
        assert(reason.has_value() && "a root trail row has no reason");
        units.push_back(
            RawUnit{lift_row(root.span().row(root.order_map()[t]), active_var_to_original_,
                             original_num_vars_),
                    *reason});
    }
    proof_writer_->write_compaction(units);
}

void Solver::backjump_to(std::size_t level) {
    assert(level <= trail_.level() && "backjump target is above the current level");
    trail_.resize(level + 1);
    decision_clause_of_level_.resize(level);
}

void Solver::make_decision_step() {
    Equation decision_eqn =
        decisions_->make_decision(trail_.span(), selector_->clauses(), selector_->active_flags());
    [[maybe_unused]] const ReduceResult reduced = trail_.span().partial_reduce(decision_eqn);
    assert(decision_eqn.num_bits() == active_num_vars_ + 1);
    // A decision must be neither 0 (already implied) nor 1 (already refuted).
    assert(reduced.kind == ReduceResult::Kind::leading &&
           "the decision procedure returned an equation the trail already decides");

    trail_.learn(decision_eqn, std::nullopt);
    decision_clause_of_level_.push_back(decisions_->decision_clause_hint());
    assert(decision_clause_of_level_.size() == trail_.level() &&
           "the decision clause record fell out of step with the trail");
    selector_->update_with_decision(decision_eqn, trail_);
    selector_->update_with_decision_clause(decisions_->decision_clause_hint());
}

void Solver::maybe_report() {
    if (!reports_progress()) {
        return;
    }
    if (counters_.num_conflicts - conflicts_at_last_report_ < progress_.conflict_interval) {
        return;
    }
    conflicts_at_last_report_ = counters_.num_conflicts;
    progress_.reporter->report(progress_snapshot());
}

ProgressSnapshot Solver::progress_snapshot() const {
    const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - solve_start_;
    return ProgressSnapshot{.time_s = elapsed.count(),
                            .num_conflicts = counters_.num_conflicts,
                            .num_propagations = counters_.num_propagations,
                            .num_restarts = counters_.num_restarts,
                            .decisions = decisions_->decision_stats().decisions,
                            .trail_level = trail_.level(),
                            .trail_rank = trail_.span().rows(),
                            .remaining_vars = active_num_vars_ - trail_[0].span().rows(),
                            .num_deletion_passes = counters_.num_deletion_passes,
                            .num_deleted_clauses = counters_.num_deleted_clauses,
                            .num_demoted_clauses = counters_.num_demoted_clauses,
                            .num_promoted_clauses = counters_.num_promoted_clauses};
}

bool Solver::maybe_compact_before_decision() {
    const CompactionState state{
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
    if (!should_compact_before_decision(state)) {
        return false;
    }

    const CompactionTransform transform = build_compaction_transform(trail_, active_num_vars_);
    if (transform.new_num_vars() == active_num_vars_) {
        // Nothing to eliminate: a plan would rewrite every clause into itself.
        return false;
    }

    apply_compaction(build_compaction_plan(state, transform));
    return true;
}

void Solver::apply_compaction(CompactionPlan plan) {
    // The root trail is about to go, and the proof needs every unit it holds.
    if (proof_writer_ != nullptr) {
        write_compaction_units();
    }

    const std::size_t num_clauses = selector_->num_clauses();
    assert(plan.rewritten_clauses.size() == num_clauses);
    assert(plan.clause_is_active.size() == num_clauses);

    for (ClauseId clause_id = 0; clause_id < num_clauses; ++clause_id) {
        selector_->clause(clause_id) = std::move(plan.rewritten_clauses[clause_id]);
        selector_->set_clause_active(clause_id, plan.clause_is_active[clause_id] != 0);
    }

    // Composed through the numbering being retired, not replaced.
    const std::size_t new_num_vars = plan.transform.new_num_vars();
    std::vector<std::size_t> compacted_var_to_original(new_num_vars);
    for (std::size_t active_var = 0; active_var < new_num_vars; ++active_var) {
        compacted_var_to_original[active_var] =
            active_var_to_original_[plan.transform.to_old(active_var)];
    }

    active_num_vars_ = new_num_vars;
    active_var_to_original_ = std::move(compacted_var_to_original);
    compaction_history_.push_back(std::move(plan.history_entry));

    propagator_.reconfigure(active_num_vars_);
    false_equation_ = Equation::eq_false(repr_, active_num_vars_);
    conflict_check_eqn_.resize(active_num_vars_);
    conflict_check_eqn_.set_eq_true();

    // The root trail is thrown away on purpose; see the warning on this method.
    trail_ = Trail(repr_, active_num_vars_);
    decision_clause_of_level_.clear();

    selector_->update_with_compaction(plan.transform, trail_);
    decisions_->update_with_compaction(plan.transform, trail_.span(), selector_->clauses(),
                                       selector_->active_flags());
    has_compacted_once_ = true;
    conflicts_at_last_compaction_ = counters_.num_conflicts;
}

void Solver::note_reason_use(ClauseId id, std::size_t lbd) {
    selector_->set_clause_used_since_deletion(id, true);
    // A starting clause is tiered by its uses too, so this does not check for a
    // learned one; its LBD starts unset, so the first use always records.
    if (lbd >= selector_->clause_lbd(id)) {
        return;
    }
    selector_->set_clause_lbd(id, lbd);

    // `ClauseTier` runs longest-lived first, so a smaller tier is a promotion.
    const ClauseTier tier = selector_->tier_for_lbd(lbd);
    if (tier < selector_->clause_tier(id)) {
        selector_->set_clause_tier(id, tier);
        selector_->set_clause_unused_intervals(id, 0);
        ++counters_.num_promoted_clauses;
    }
}

void Solver::maybe_delete_clauses() {
    if (!deletion_.enabled) {
        return;
    }
    if (counters_.num_conflicts - conflicts_at_last_deletion_ < deletion_.conflict_interval) {
        return;
    }
    conflicts_at_last_deletion_ = counters_.num_conflicts;
    apply_deletion(build_deletion_plan(DeletionState{.selector = *selector_,
                                                     .trail = trail_,
                                                     .key = deletion_.key,
                                                     .activity = deletion_.activity}));
}

void Solver::apply_deletion(const DeletionPlan& plan) {
    const std::size_t num_clauses = selector_->num_clauses();
    assert(plan.unused_intervals.size() == num_clauses);

    for (ClauseId clause_id = 0; clause_id < num_clauses; ++clause_id) {
        selector_->set_clause_unused_intervals(clause_id, plan.unused_intervals[clause_id]);
        selector_->set_clause_used_since_deletion(clause_id, false);
    }
    for (const ClauseId clause_id : plan.demoted) {
        selector_->set_clause_tier(clause_id, ClauseTier::local);
    }

    if (proof_writer_ != nullptr && !plan.evicted.empty()) {
        proof_writer_->write_deleted(plan.evicted);
    }
    for (const ClauseId clause_id : plan.evicted) {
        selector_->clause(clause_id) = inactive_clause_placeholder(repr_, false_equation_);
        selector_->set_clause_active(clause_id, false);
    }

    ++counters_.num_deletion_passes;
    counters_.num_deleted_clauses += plan.evicted.size();
    counters_.num_demoted_clauses += plan.demoted.size();
}

Solution Solver::solve() {
#ifndef NDEBUG
    has_solved_ = true;
#endif
    next_proof_clause_id_ = std::max(next_proof_clause_id_, selector_->num_clauses());

    solve_start_ = std::chrono::steady_clock::now();
    if (reports_progress()) {
        progress_.reporter->report(progress_snapshot());
    }

    while (true) {
        if (propagate_until_conflict()) {
            ++counters_.num_conflicts;
            if (level() == 0) {
                write_root_unsat_proof();
                return Solution{SolveResult::unsat, std::nullopt};
            }
            handle_non_root_conflict();
            maybe_delete_clauses();
            maybe_report();
            continue;
        }

        // Quiescent at full rank: the trail is a total assignment.
        if (trail_.span().rows() == active_num_vars_) {
            const IndexSet active_assignment = trail_.span().unique_solution();
            IndexSet original_assignment = reconstruct_original_assignment(
                active_assignment, compaction_history_, active_var_to_original_,
                original_num_vars_);
            return Solution{SolveResult::sat, std::move(original_assignment)};
        }

        if (compaction_.enabled && maybe_compact_before_decision()) {
            // The active numbering changed, so re-propagate rather than decide.
            continue;
        }

        make_decision_step();
    }
}

} // namespace xorcle
