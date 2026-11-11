#include "core/solver.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "algebra/reduce_result.h"
#include "core/analysis.h"
#include "core/trail_basis.h"
#include "heuristics/compaction/original_variables.h"

namespace xorcle {

#ifndef NDEBUG
namespace {

bool debug_rows_are_reduced(const RowSpace& clause) {
    RowSpace reduced = clause;
    reduced.reduce_rows();
    for (std::size_t row_idx = 0; row_idx < clause.rows(); ++row_idx) {
        if (!(clause.row(row_idx) == reduced.row(row_idx))) {
            return false;
        }
    }
    return true;
}

} // namespace
#endif

Solver::Solver(EquationRepr repr, std::size_t num_vars, std::vector<std::size_t> used_vars,
               const ClauseSelectorFactory& make_selector,
               std::unique_ptr<Csids> decisions,
               std::unique_ptr<RestartPolicy> restarts, ProofWriter* proof_writer,
               const SolverConfig& config)
    : repr_(repr),
      original_num_vars_(num_vars),
      used_vars_(std::move(used_vars)),
      active_num_vars_(used_vars_.size()),
      active_var_to_original_(used_vars_),
      dropped_col_idxs_(active_num_vars_ + 1),
      renumber_(config.renumber),
      deletion_(config.deletion),
      trail_(repr, active_num_vars_),
      selector_(make_selector(clause_database_)),
      decisions_(std::move(decisions)),
      restarts_(std::move(restarts)),
      propagator_(repr, active_num_vars_),
      proof_writer_(proof_writer),
      proof_root_span_(repr, num_vars),
      attempt_budget_config_(config.attempt_budget),
      propagated_row_eqn_(repr, active_num_vars_),
      false_equation_(Equation::eq_false(repr, active_num_vars_)),
      satisfied_probe_eqn_(repr, active_num_vars_),
      replay_implied_eqn_(repr, active_num_vars_),
      replay_reduced_eqn_(repr, active_num_vars_),
      progress_(config.progress),
      minimize_(config.minimize),
      bump_responsible_clauses_(config.bump_responsible_clauses),
      otf_subsume_(config.otf_subsume),
      lbd_window_(config.lbd_window),
      bve_config_(config.bve),
      bve_is_due_(config.bve.has_value()),
      implication_graph_config_(config.implication_graph),
      vivify_(config.vivify),
      subsume_pass_(config.inproc_subsume_strengthen) {
    assert(std::ranges::is_sorted(used_vars_, std::less_equal<>{}) &&
           (used_vars_.empty() || used_vars_.back() < num_vars) &&
           "the used variables must be strictly ascending and below num_vars");
    assert(selector_ != nullptr && "the clause selector must not be null");
    assert(decisions_ != nullptr && "the decision procedure must not be null");
    assert(restarts_ != nullptr && "the restart policy must not be null; use NoRestarts");
    if (config.stabilize.has_value()) {
        focused_restarts_.emplace(config.stabilize->focused_restarts);
        modes_.emplace(config.stabilize->interval_conflicts);
    }
    cyclic_selector_ = dynamic_cast<Cyclic*>(selector_.get());
    assert((!attempt_budget_config_.has_value() || cyclic_selector_ != nullptr) &&
           "an attempt budget needs the cyclic selector");
    assert((proof_writer_ == nullptr || proof_writer_->num_vars() == original_num_vars_) &&
           "the proof writer's variable numbering must be the instance's");
#ifdef NDEBUG
    tracked_used_row_idxs_ = attempt_budget_config_.has_value() ? &used_row_idxs_ : nullptr;
#else
    // Debug checks the attribution whether or not a budget is on.
    tracked_used_row_idxs_ = &used_row_idxs_;
#endif
    // The run's seed lives on the decision procedure, so these wait for its null check.
    representative_rng_ = make_rng(decisions_->base_seed(), RngComponent::minimize_representative);
    if (implication_graph_config_.has_value()) {
        implication_graph_.emplace(repr, active_num_vars_, *implication_graph_config_);
        implication_graph_->set_base_seed(decisions_->base_seed());
    }
    bve_rng_ = make_rng(decisions_->base_seed(), RngComponent::bve_draw);
    if (proof_writer_ != nullptr) {
        proof_col_idx_to_unit_id_.resize(num_vars);
    }
    if (vivify_.has_value()) {
        assert(vivify_->rounds >= 1 && vivify_->effort >= 0.0 &&
               "a vivification pass has no round, or its effort is negative");
        vivify_rng_ = make_rng(decisions_->base_seed(), RngComponent::vivify_basis);
    }
    if (subsume_pass_.has_value()) {
        subsume_pass_rng_ = make_rng(decisions_->base_seed(), RngComponent::subsume_pass_draw);
    }
}

void Solver::add_starting_clause(Clause clause) {
    assert(!has_solved_ && "input clauses must be added before solving");
    assert(compaction_history_.empty() && "input clauses must be added before any compaction");
    assert(clause.cols() == active_num_vars_ + 1 &&
           "a starting clause must be over the used variables");
    assert(clause.repr() == repr_ &&
           "a starting clause must use the solver's representation");

    if (clause.rows() == 0) {
        if (!empty_starting_clause_proof_id_) {
            empty_starting_clause_proof_id_ = next_proof_clause_id_;
        }
        if (proof_writer_ != nullptr) {
            ++next_proof_clause_id_;
        }
        return;
    }
    if (clause.pivot_col_idxs().test(clause.num_vars())) {
        // The linear negation holds `0 = 1`, so every assignment satisfies the clause, which
        // keeps its place in the proof's numbering.
        ++stats_.num_tautologies_at_input;
        if (proof_writer_ != nullptr) {
            ++next_proof_clause_id_;
        }
        return;
    }

    clause.reduce_rows();
    const ClauseId new_clause_id = clause_database_.num_clauses();
    if (equivalent_clauses_
            .find_or_enter(clause, clause.rows_hash(active_var_to_original_), new_clause_id,
                           clause_database_.clauses())
            .has_value()) {
        // The clause stays out of the database and keeps its place in the proof's numbering.
        ++stats_.num_equivalent_clauses;
        if (proof_writer_ != nullptr) {
            ++next_proof_clause_id_;
        }
        return;
    }
    decisions_->update_with_new_clause(clause, new_clause_id);
    add_starting_clause_to_database(std::move(clause));
    ++num_active_clauses_;
    if (proof_writer_ != nullptr) {
        proof_id_of_clause_.push_back(next_proof_clause_id_++);
    }
}

void Solver::add_starting_clause_to_database(Clause clause) {
    selector_->update_with_new_clause(clause_database_.add_starting_clause(std::move(clause)));
}

void Solver::add_asserting_clause_to_database(Clause clause, const AssertingClauseInfo& info) {
    const ClauseId id = clause_database_.add_asserting_clause(std::move(clause), info.lbd);
    selector_->update_with_asserting_clause(id, trail_, info);
}

bool Solver::propagate_until_conflict() {
    assert(active_var_to_original_.size() == active_num_vars_ &&
           "the variable map is out of step with the active numbering");
    assert(false_equation_.num_bits() == active_num_vars_ + 1 &&
           "0 = 1 is over another numbering than the active one");
    assert(propagated_row_eqn_.num_bits() == active_num_vars_ + 1 &&
           "the propagation scratch is over another numbering than the active one");

    const std::size_t budget = attempt_budget();
    if (cyclic_selector_ != nullptr) {
        cyclic_selector_->set_attempt_budget(budget);
    }
    force_complete_sweep_ = false;

    while (true) {
        const std::optional<ClauseSelection> selection = selector_->next();
        if (!selection.has_value()) {
            assert((budget_.has_value() || debug_nothing_left_to_propagate()) &&
                   "propagation stopped short of a fixed point with no budget");
            return false;
        }
        assert(selection->id < clause_database_.num_clauses() &&
               "selector returned an unknown clause");
        // A clause an earlier level already satisfies can only tell us so again.
        if (clause_is_known_satisfied(clause_database_.record(selection->id))) {
            continue;
        }
        if (propagate_from_clause(selection->id, selection->trigger_hint) ==
            PropagationOutcome::conflicting) {
            return true;
        }
    }
}

Solver::PropagationOutcome Solver::propagate_from_clause(ClauseId id,
                                                         std::optional<WatchType> trigger_hint,
                                                         bool count_learn_as_missed) {
    assert(clause_database_.is_clause_active(id) && "propagating from an inactive clause");
    const Clause& clause = clause_database_.clause(id);
    ClauseRecord& record = clause_database_.record(id);
    ++propagation_counters_.num_attempt_props;

    const std::size_t current_level = trail_.level();
    // A propagation holds at the current level or below, so at the root there is
    // nothing an attribution could find.
    const bool attribute = tracked_used_row_idxs_ != nullptr && current_level != 0;

    Equation* propagated_eqn = propagator_.unit_propagate_one_clause(
        id, clause, record.watches, trail_, trigger_hint, propagation_counters_.num_watch_updates,
        *selector_, attribute ? tracked_used_row_idxs_ : nullptr);
    if (propagated_eqn == nullptr) {
        if (propagator_.last_attempt_found_satisfied()) {
            const std::size_t satisfied_level =
                shallowest_satisfying_level(clause.row(propagator_.last_satisfying_row_idx()));
            record.satisfied_level = satisfied_level;
            record.satisfied_stamp = level_stamp_[satisfied_level];
        }
        return PropagationOutcome::nothing;
    }

    assert(propagated_eqn->num_bits() == active_num_vars_ + 1 &&
           "a propagated equation is over another numbering than the active one");

    std::size_t level = current_level;
    if (attribute) {
        const std::size_t true_level = level_of_used_rows(used_row_idxs_);
        assert((budget_.has_value() || true_level == current_level) &&
               "quiescent propagation derived an equation implied at a shallower level");
        if (budget_.has_value()) {
            level = true_level;
        }
    }

    // The propagated equation is `0 = 1` exactly when the clause is falsified.
    const bool conflicting = propagated_eqn->is_eq_false();
    assert((!conflicting || !count_learn_as_missed) &&
           "the clause was falsified again after the level falsifying it was undone");
    bool missed = count_learn_as_missed;
    if (conflicting && in_vivify_try_ && budget_.has_value()) {
        // A try's assumed rows conflict however late the conflict is found, and
        // none of them is taken off.
        missed = level < current_level ||
                 (level > 0 && falsified_only_at_top_level(clause));
        level = current_level;
    }
    if (conflicting && level < current_level) {
        // The levels above hold nothing the analysis may keep, so discarding
        // them here lets the `0 = 1` go in at the level it holds at.
        backjump_to(level);
    }
    // A clause only the top level falsifies was propagating before that level's
    // decision was taken, so the decision goes and the clause is met again on
    // the trail it was missed on.
    if (conflicting && level > 0 && budget_.has_value() && !in_vivify_try_ &&
        falsified_only_at_top_level(clause)) {
        ++stats_.num_undone_decisions;
        backjump_to(level - 1);
        return propagate_from_clause(id, std::nullopt, true);
    }

    ++propagation_counters_.num_propagations;
    const bool below_current_level = level < current_level;
    propagation_counters_.num_out_of_order_learns += below_current_level ? 1 : 0;
    if (budget_.has_value()) {
        budget_->note_learn(below_current_level || missed);
    }

    const Equation* learned_eqn = propagated_eqn;
    if (below_current_level) {
        // The propagator reduced against the levels above `level` too, so the
        // raw row goes in and `Trail::learn` reduces it against the rows at
        // `level` and below.
        propagated_row_eqn_.assign_from(clause.row(propagator_.last_propagating_row_idx()));
        propagated_row_eqn_.negate();
        learned_eqn = &propagated_row_eqn_;
    } else {
        // Re-normalize: the propagator reduced the clause row it propagated
        // from, not the negation it returns, so the result can still hold
        // span rows.
        (void)trail_.span().partial_reduce(*propagated_eqn);
    }
    const std::size_t trail_idx = trail_.learn(*learned_eqn, id, level);
    assert(debug_reason_implies(id, trail_idx) &&
           "a propagated equation does not follow from its reason");
    // The selector is notified even on the propagation that conflicts, and
    // with the row as stored, which is the equation reduced by its own level.
    selector_->update_with_new_equation(trail_.row(trail_idx), trail_);

    if (conflicting) {
        conflict_clause_id_ = id;
        conflict_level_ = level;
        return PropagationOutcome::conflicting;
    }
    if (proof_writer_ != nullptr && level == 0) {
        write_root_unit(id, trail_idx);
    }
    return PropagationOutcome::propagated;
}

std::size_t Solver::attempt_budget() {
    if (!budget_.has_value() || force_complete_sweep_ || trail_.level() == 0) {
        return 0;
    }
    const std::size_t num_clauses = std::max<std::size_t>(1, clause_database_.num_clauses());
    budget_->set_num_clauses(num_clauses);
    const std::size_t attempts = budget_->attempts();
    // A budget the length of a whole sweep is no budget: only quiescence can end
    // such a run.
    return attempts >= num_clauses ? 0 : attempts;
}

std::optional<Solution> Solver::handle_conflict() {
    ++stats_.num_conflicts;
    if (replay_.is_pending()) {
        stats_.num_replays_ended_by_conflict += replay_.has_replayed ? 1U : 0U;
        end_replay();
    }
    assert(trail_.level() == conflict_level_ &&
           "the conflicting propagation left the trail above the level the conflict holds at");
    if (level() == 0) {
        phase_timer_.switch_to(SolverPhase::analysis);
        write_root_unsat_proof();
        return Solution{SolveResult::unsat, std::nullopt};
    }
    handle_non_root_conflict();
    maybe_delete_clauses();
    maybe_report();
    return std::nullopt;
}

void Solver::handle_non_root_conflict() {
    phase_timer_.switch_to(SolverPhase::decisions);
    decisions_->update_with_conflict(
        trail_, std::span<const std::size_t>(level_stamp_).first(trail_.level() + 1));
    phase_timer_.switch_to(SolverPhase::analysis);
    AssertingClause learned = find_asserting_clause(
        trail_, clause_database_.clauses(), conflict_clause_id_, representative_rng_, minimize_,
        bump_responsible_clauses_, otf_subsume_);
    for (const ClauseId id : learned.reason_ids) {
        decisions_->update_with_resolved_clause(id);
    }
    learned.clause.reduce_rows();
    for (SubsumedReason& record : learned.subsumed_reasons) {
        if (record.replacement.has_value()) {
            record.replacement->reduce_rows();
        }
    }
    // Entered unlooked for: after propagation to a fixed point a kept clause with
    // its span would have asserted at the asserting level, and the clause learned
    // would be satisfied rather than falsified. With an attempt budget it can.
    const std::size_t learned_key = learned.clause.rows_hash(active_var_to_original_);
    assert((budget_.has_value() ||
            !equivalent_clauses_.find(learned.clause, learned_key, clause_database_.clauses())
                 .has_value()) &&
           "a kept clause has the learned clause's span after propagation to a fixed point");
    if (minimize_) {
        stats_.minimize_width_before += learned.width_before_minimize;
        stats_.minimize_width_after += learned.clause.rows();
        stats_.num_minimized_clauses +=
            learned.clause.rows() < learned.width_before_minimize ? 1U : 0U;
    }
    // Before the proof path reverses `reason_ids`, which `reason_lbds` parallels.
    for (std::size_t i = 0; i < learned.reason_ids.size(); ++i) {
        ClauseRecord& record = clause_database_.record(learned.reason_ids[i]);
        record.add_lbd_measurement(learned.reason_lbds[i], lbd_window_);
        if (deletion_.has_value()) {
            update_with_reason_use(record);
        }
    }
    // With the resolved-clause bumps, before the decay below.
    if (bump_responsible_clauses_) {
        bump_responsible_clauses(learned.responsible_trail_idxs);
    }

    // The id the clause is *about* to take, read before it is added.
    const ClauseId new_clause_id = clause_database_.num_clauses();

    // Records the activity the new clause will enter at, and decays. Must precede
    // the `update_with_new_clause()` below, which spends that recorded activity.
    decisions_->update_with_asserting_clause(learned.clause);

    assert((learned.clause.rows() != 1 || learned.asserting_level == 0) &&
           "a unit asserting clause must assert at the root");
    const std::size_t level_before_backjump = trail_.level();
    // Sampled at the conflict, before the backjump.
    sum_learned_lbd_ += learned.lbd;
    ++num_asserting_clauses_;
    sum_conflict_level_ += level_before_backjump;
    sum_conflict_trail_length_ += trail_.span().rows();
    // Consumes a conflict from the restart budget, so it must run exactly once
    // per non-root conflict whether or not a restart fires.
    const bool focused = modes_.has_value() && modes_->mode() == SearchMode::focused;
    RestartPolicy& restarts = focused ? *focused_restarts_ : *restarts_;
    const bool restart_now = restarts.should_restart_after_conflict(
        ConflictSummary{.lbd = learned.lbd, .trail_length = trail_.span().rows()});

    // How many levels the *solver* discards. Without a restart that is the
    // complement of the asserting level; with one it is every non-root level.
    std::size_t removed_levels = level_before_backjump - learned.asserting_level;
    backjump_to(learned.asserting_level);
    if (restart_now) {
        removed_levels = level_before_backjump;
        backjump_to(0);
        restarts.note_restart();
        ++stats_.num_restarts;
        stats_.focused_restarts += focused ? 1U : 0U;
    }
    if (modes_.has_value() && modes_->update_with_conflict()) {
        switch_mode();
    }

    decisions_->update_with_new_clause(learned.clause, new_clause_id);

    // Each replacement's chain: the subsumed reason itself, then the derivation
    // prefix before it, which is replay order; sliced before the in-place
    // reversal below. A subsumed conflicting clause already opens the prefix,
    // so its chain is the plain reversed prefix.
    std::vector<std::vector<ClauseId>> replacement_chains(learned.subsumed_reasons.size());
    if (proof_writer_ != nullptr) {
        for (std::size_t i = 0; i < learned.subsumed_reasons.size(); ++i) {
            const SubsumedReason& record = learned.subsumed_reasons[i];
            if (record.replacement.has_value()) {
                if (!record.subsumed_conflicting_clause) {
                    replacement_chains[i].push_back(record.clause_id);
                }
                const std::size_t prefix = record.num_reasons -
                                           (record.subsumed_conflicting_clause ? 0 : 1);
                replacement_chains[i].insert(replacement_chains[i].end(),
                                             learned.reason_ids.rend() -
                                                 static_cast<std::ptrdiff_t>(prefix),
                                             learned.reason_ids.rend());
            }
        }
    }

    if (proof_writer_ != nullptr) {
        // Derivation order to replay order. Exactly one reversal, here.
        std::reverse(learned.reason_ids.begin(), learned.reason_ids.end());
        assert(proof_id_of_clause_.size() == new_clause_id &&
               "a clause went without a proof id");
        proof_id_of_clause_.push_back(
            write_proof_line(learned.clause, proof_ids(learned.reason_ids)));
    }

    const AssertingClauseInfo learned_info{.lbd = learned.lbd, .removed_levels = removed_levels};
    stats_.learned_width_1 += learned.clause.rows() == 1 ? 1U : 0U;
    stats_.learned_width_2 += learned.clause.rows() == 2 ? 1U : 0U;
    add_asserting_clause_to_database(std::move(learned.clause), learned_info);
    ++num_active_clauses_;
    equivalent_clauses_.enter(new_clause_id, learned_key);

    // A reason a trail row still names is left as is, replacement included.
    const auto reason_still_named = [this](const SubsumedReason& record) {
        const std::vector<std::optional<ClauseId>>& reasons = trail_.reasons();
        const bool named = record.trail_idx < reasons.size() &&
                           reasons[record.trail_idx] == record.clause_id;
        assert(named == (std::find(reasons.begin(), reasons.end(),
                                   std::optional<ClauseId>{record.clause_id}) != reasons.end()) &&
               "a clause is the reason of a row other than its recorded one");
        return named;
    };

    // On-the-fly subsumption: each record's resolvent implies its reason clause.
    // Add every resolvent (unless it is the clause just learned) before any
    // deletion, because a later record's chain names an earlier record's reason.
    for (std::size_t i = 0; i < learned.subsumed_reasons.size(); ++i) {
        SubsumedReason& record = learned.subsumed_reasons[i];
        assert(clause_database_.is_clause_active(record.clause_id) &&
               "a conflict reason must be active");
        if (reason_still_named(record)) {
            continue;
        }
        // What replaces an input clause is no more a deletion candidate than the
        // input clause was, and so on along chains of replacements.
        const bool replaces_input = !clause_database_.record(record.clause_id).learned;
        ClauseId replacement_id = new_clause_id;
        std::optional<ClauseId> kept_id;
        if (record.replacement.has_value()) {
            kept_id = equivalent_clauses_.find_or_enter(
                *record.replacement, record.replacement->rows_hash(active_var_to_original_),
                clause_database_.num_clauses(), clause_database_.clauses());
            // A replacement is falsified by the trail below the conflict and a subsumed
            // reason satisfied by it; the subsumed conflicting clause uses a trail row
            // that no replacement uses.
            assert((!kept_id.has_value() ||
                    std::ranges::none_of(learned.subsumed_reasons,
                                         [&](const SubsumedReason& other) {
                                             return other.clause_id == *kept_id &&
                                                    !reason_still_named(other);
                                         })) &&
                   "a clause this conflict's subsumption removes has a replacement's span");
        }
        if (kept_id.has_value()) {
            // The kept clause is the replacement, so none is added.
            replacement_id = *kept_id;
            ++stats_.num_equivalent_clauses;
            merge_deletion_state(
                *kept_id,
                EvictedClauseState{.is_learned = !replaces_input,
                                   .lbd = static_cast<double>(record.lbd),
                                   .unused_passes = 0,
                                   .is_used_since_deletion = false});
        } else if (record.replacement.has_value()) {
            replacement_id = clause_database_.num_clauses();
            decisions_->update_with_new_clause(*record.replacement, replacement_id);
            if (proof_writer_ != nullptr) {
                proof_id_of_clause_.push_back(
                    write_proof_line(*record.replacement, proof_ids(replacement_chains[i])));
            }
            add_asserting_clause_to_database(
                std::move(*record.replacement),
                AssertingClauseInfo{.lbd = record.lbd,
                                    .removed_levels = removed_levels,
                                    .is_replacement = true});
            ++num_active_clauses_;
        }
        if (replaces_input) {
            clause_database_.record(replacement_id).learned = false;
        }
        decisions_->update_with_subsumed_clause(record.clause_id, replacement_id);
    }
    for (const SubsumedReason& record : learned.subsumed_reasons) {
        if (reason_still_named(record)) {
            continue;
        }
        remove_clause(record.clause_id);
        ++stats_.num_subsumed_clauses;
    }
    assert(debug_no_two_clauses_share_a_span() && "two active clauses have one span");
}

void Solver::write_root_unsat_proof() {
    if (proof_writer_ == nullptr) {
        return;
    }
    proof_writer_->write_empty(
        next_proof_clause_id_++,
        proof_ids(find_root_conflict_reasons(trail_, clause_database_.clauses(),
                                             conflict_clause_id_, representative_rng_)));
}

void Solver::write_root_unit(ClauseId reason_id, std::size_t trail_idx) {
    assert(proof_writer_ != nullptr && "no proof to record a root unit in");
    assert(trail_.level_of(trail_idx) == 0 && "the recorded row is not a root unit");

    // Reduced by the units before it, the row is what this line proves, so its
    // pivot can name the line.
    const ProofRow trail_row =
        lift_row(trail_.row(trail_idx), active_var_to_original_, original_num_vars_);
    Equation unit = Equation::from_bits(repr_, original_num_vars_, trail_row);
    proof_root_span_.reduce(unit);
    Equation negated_unit = unit;
    negated_unit.negate();

    // Every reason row reduces to `0 = 0` or to the unit's linear negation, so
    // the units those reductions used are all the line needs before its reason.
    const Clause& reason = clause_database_.clause(reason_id);
    IndexSet used(proof_root_span_.rows());
    Equation reason_row(repr_, original_num_vars_);
    for (const Equation& row : reason) {
        reason_row.assign_bits(lift_row(row, active_var_to_original_, original_num_vars_));
        const IndexSet coefficients = proof_root_span_.reduce_get_coefficients(reason_row);
        assert((reason_row.is_eq_true() || reason_row == negated_unit) &&
               "the reason clause does not propagate the root unit");
        coefficients.for_each_set([&used](std::size_t row_idx) { used.set(row_idx); });
    }

    // A one-row reason needing no unit already is the line.
    ClauseId unit_id = proof_id_of_clause_[reason_id];
    if (reason.rows() != 1 || !used.none()) {
        std::vector<ClauseId> reasons = root_unit_lines(used);
        reasons.push_back(unit_id);
        unit_id = next_proof_clause_id_++;
        const ProofRow negated_row = negated_unit.bits();
        proof_writer_->write_learned(unit_id, std::span<const ProofRow>(&negated_row, 1), reasons);
        proof_unit_only_lines_.push_back(unit_id);
    }

    const std::optional<std::size_t> row_idx =
        proof_root_span_.add_row_get_row_idx(std::move(unit));
    assert(row_idx.has_value() && "a root unit is in the span of the ones before it");
    proof_col_idx_to_unit_id_[*proof_root_span_.row(*row_idx).leading_bit()] = unit_id;
    proof_root_unit_ids_.push_back(unit_id);
}

std::vector<ClauseId> Solver::root_unit_lines(const IndexSet& used) const {
    std::vector<ClauseId> lines;
    used.for_each_set([this, &lines](std::size_t row_idx) {
        lines.push_back(proof_col_idx_to_unit_id_[*proof_root_span_.row(row_idx).leading_bit()]);
    });
    return lines;
}

ClauseId Solver::write_substitution_line(const Clause& clause, const Clause& rewritten,
                                         const ClauseId line) {
    // Each row reduces by the units to the rewritten clause's span, so under the
    // rewritten clause's linear negation the units it used and then the clause conflict.
    IndexSet used(proof_root_span_.rows());
    Equation lifted(repr_, original_num_vars_);
    for (const Equation& row : clause) {
        lifted.assign_bits(lift_row(row, active_var_to_original_, original_num_vars_));
        proof_root_span_.reduce_get_coefficients(lifted).for_each_set(
            [&used](std::size_t row_idx) { used.set(row_idx); });
    }
    assert(!used.none() && "a clause the substitution rewrote uses no root unit");
    std::vector<ClauseId> reasons = root_unit_lines(used);
    reasons.push_back(line);
    const ClauseId rewritten_line = next_proof_clause_id_++;
    proof_writer_->write_learned(rewritten_line, rewritten, active_var_to_original_, reasons);
    return rewritten_line;
}

std::vector<ClauseId> Solver::proof_ids(const std::vector<ClauseId>& clause_ids) const {
    std::vector<ClauseId> ids;
    ids.reserve(clause_ids.size());
    for (const ClauseId id : clause_ids) {
        ids.push_back(proof_id_of_clause_[id]);
    }
    return ids;
}

std::size_t Solver::shallowest_satisfying_level(const Equation& row) {
    satisfied_probe_eqn_.assign_from(row);
    trail_.span().reduce_get_coefficients(satisfied_probe_eqn_, satisfied_probe_row_idxs_);
    assert(satisfied_probe_eqn_.is_eq_false() && "the row does not reduce to 0 = 1");
    return level_of_used_rows(satisfied_probe_row_idxs_);
}

std::size_t Solver::level_of_used_rows(const IndexSet& used_row_idxs) const {
    assert(used_row_idxs.size() == trail_.span().rows() &&
           "the used rows were collected against a different span");
    const std::vector<std::size_t>& row_idx_to_trail_idx = trail_.row_idx_to_trail_idx();
    std::size_t deepest_trail_idx = 0;
    bool used_any = false;
    used_row_idxs.for_each_set([&](const std::size_t row_idx) {
        const std::size_t trail_idx = row_idx_to_trail_idx[row_idx];
        deepest_trail_idx = used_any ? std::max(deepest_trail_idx, trail_idx) : trail_idx;
        used_any = true;
    });
    // An equation needing nothing from the trail holds at the root.
    return used_any ? trail_.level_of(deepest_trail_idx) : 0;
}

bool Solver::falsified_only_at_top_level(const Clause& clause) const {
    const std::size_t dimension =
        TrailBasisSpace::from_RowSpace(clause, trail_).dimension_at_top_level();
    // The trail stands at the level the falsification is dated to, so the levels
    // below it leave the clause a dimension to propagate.
    assert(dimension > 0 && "the levels below the top one already falsify the clause");
    return dimension == 1;
}

void Solver::backjump_to(std::size_t level) {
    assert(level <= trail_.level() && "backjump target is above the current level");
    trail_.resize(level + 1);
    selector_->update_with_backjump(level, trail_);
    decision_clause_of_level_.resize(level);
    decisions_->update_with_backjump(level);
    did_complete_sweep_ = false;
}

Solver::DecisionStep Solver::make_decision_step() {
    phase_timer_.switch_to(SolverPhase::decisions);
    // The last propagation run ended without a conflict, so how it stopped says
    // whether it reached quiescence.
    const SatisfactionRecord known(clause_database_.records(), level_stamp_, trail_.level(),
                                   !last_run_stopped_on_budget());
    DecisionResult decision = decisions_->make_decision(trail_.span(), clause_database_.clauses(),
                                                        clause_database_.active_flags(), known);
    if (decision.propagating_clause.has_value()) {
        propagating_clause_id_ = *decision.propagating_clause;
        return DecisionStep::propagating;
    }
    if (!decision.equation.has_value()) {
        return DecisionStep::exhausted;
    }
    Equation decision_eqn = *std::move(decision.equation);
    [[maybe_unused]] const ReduceResult reduced = trail_.span().partial_reduce(decision_eqn);
    assert(decision_eqn.num_bits() == active_num_vars_ + 1 &&
           "a decision is over another numbering than the active one");
    // A decision must be neither `0 = 0` (already implied) nor `0 = 1` (already refuted).
    assert(reduced.kind == ReduceResult::Kind::leading &&
           "the decision procedure returned an equation the trail already decides");

    const std::optional<ClauseId> decision_clause = decisions_->decision_clause_hint();
    phase_timer_.switch_to(SolverPhase::propagation);
    open_level(decision_eqn, decision_clause);
    return DecisionStep::decided;
}

void Solver::open_level(const Equation& reduced_eqn, std::optional<ClauseId> decision_clause) {
    trail_.learn(reduced_eqn, std::nullopt, trail_.level() + 1);
    level_stamp_.resize(trail_.level() + 1);
    level_stamp_[trail_.level()] = next_level_stamp_++;
    decision_clause_of_level_.push_back(decision_clause);
    assert(decision_clause_of_level_.size() == trail_.level() &&
           "the decision clause record fell out of step with the trail");
    selector_->update_with_decision(reduced_eqn, trail_, decision_clause);
}

#ifndef NDEBUG
bool Solver::every_active_clause_is_satisfied(const RowSpace& model) const {
    Equation reduced(repr_, active_num_vars_);
    for (ClauseId id = 0; id < clause_database_.num_clauses(); ++id) {
        if (!clause_database_.is_clause_active(id)) {
            continue;
        }
        bool satisfied = false;
        for (const Equation& row : clause_database_.clause(id)) {
            reduced.assign_from(row);
            // A row the model refutes is an equation of the clause that holds.
            satisfied = model.partial_reduce(reduced).kind == ReduceResult::Kind::eq_false;
            if (satisfied) {
                break;
            }
        }
        if (!satisfied) {
            return false;
        }
    }
    return true;
}

bool Solver::debug_reason_implies(const ClauseId id, const std::size_t trail_idx) {
    const std::size_t num_rows = trail_.rows_at_level(trail_.level_of(trail_idx));
    const std::size_t words = active_num_vars_ / 64 + 1;
    if (++debug_reason_checks_ % (1 + num_rows * words / 256) != 0) {
        return true;
    }
    RowSpace span(repr_, active_num_vars_);
    for (const Equation& row : clause_database_.clause(id)) {
        span.add_row(row);
    }
    for (std::size_t other_idx = 0; other_idx < num_rows; ++other_idx) {
        if (other_idx != trail_idx) {
            span.add_row(trail_.row(other_idx));
        }
    }
    Equation negated = trail_.row(trail_idx);
    negated.negate();
    span.reduce(negated);
    return negated.is_eq_true();
}

bool Solver::debug_nothing_left_to_propagate() {
    const std::size_t words = active_num_vars_ / 64 + 1;
    const std::size_t stride = 1 + num_active_clauses_ * words * words / 2048;
    if (++debug_quiescences_ % stride != 0) {
        return true;
    }
    Equation first(repr_, active_num_vars_);
    Equation reduced(repr_, active_num_vars_);
    for (ClauseId id = 0; id < clause_database_.num_clauses(); ++id) {
        if (!clause_database_.is_clause_active(id)) {
            continue;
        }
        // Fully reduced rows are canonical, so two distinct nonzero ones span two
        // dimensions, and one alone propagates unless it is `0 = 1`.
        bool have_first = false;
        bool two_dimensions = false;
        for (const Equation& row : clause_database_.clause(id)) {
            reduced.assign_from(row);
            trail_.span().reduce(reduced);
            if (reduced.is_eq_true()) {
                continue;
            }
            if (!have_first) {
                first.assign_from(reduced);
                have_first = true;
            } else if (!(reduced == first)) {
                two_dimensions = true;
                break;
            }
        }
        if (!two_dimensions && !(have_first && first.is_eq_false())) {
            return false;
        }
    }
    return true;
}

bool Solver::active_clause_count_is_consistent() const {
    const std::vector<std::uint8_t>& active = clause_database_.active_flags();
    return num_active_clauses_ ==
           static_cast<std::size_t>(std::count(active.begin(), active.end(), 1));
}
#endif

void Solver::maybe_report() {
    if (!reports_progress()) {
        return;
    }
    if (stats_.num_conflicts - conflicts_at_last_report_ < progress_.cadence) {
        return;
    }
    phase_timer_.switch_to(SolverPhase::other);
    report_progress();
}

void Solver::report_progress() {
    const ProgressSnapshot snapshot = progress_snapshot();
    progress_.reporter->report(snapshot);
    conflicts_at_last_report_ = stats_.num_conflicts;
    sum_level_at_last_report_ = sum_conflict_level_;
    sum_trail_length_at_last_report_ = sum_conflict_trail_length_;
    time_at_last_report_s_ = snapshot.time_s;
}

ProgressSnapshot Solver::progress_snapshot() const {
    assert(active_clause_count_is_consistent() &&
           "the tracked live-clause count fell out of step with the active flags");
    const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - solve_start_;
    const double time_s = elapsed.count();

    // Everything below is taken over the interval since the last line, so a
    // reader sees the current trend rather than the run's flattening average.
    const std::size_t interval_conflicts = stats_.num_conflicts - conflicts_at_last_report_;
    const double interval_s = time_s - time_at_last_report_s_;
    const auto per_conflict = [interval_conflicts](const std::size_t interval_sum) {
        return interval_conflicts == 0 ? 0.0
                                       : static_cast<double>(interval_sum) /
                                             static_cast<double>(interval_conflicts);
    };

    return ProgressSnapshot{
        .time_s = time_s,
        .num_conflicts = stats_.num_conflicts,
        .conflicts_per_second =
            interval_s > 0.0 ? static_cast<double>(interval_conflicts) / interval_s : 0.0,
        .num_propagations = propagation_counters_.num_propagations,
        .num_clauses = num_active_clauses_,
        .average_decision_level =
            per_conflict(sum_conflict_level_ - sum_level_at_last_report_),
        .average_trail_length = per_conflict(sum_conflict_trail_length_ -
                                            sum_trail_length_at_last_report_),
        .remaining_vars = active_num_vars() - trail_.rows_at_level(0),
        .decisions = decisions_->counters().decisions,
        .decision_level = trail_.level(),
        .trail_length = trail_.span().rows(),
        .num_restarts = stats_.num_restarts,
        .num_deleted_clauses = stats_.num_deleted_clauses,
        .num_attempt_props = propagation_counters_.num_attempt_props,
        .num_watch_updates = propagation_counters_.num_watch_updates};
}

void Solver::substitute_root_units() {
    phase_timer_.switch_to(SolverPhase::substitution);
    CompactionState state{
        .trail = trail_,
        .clauses = clause_database_.clauses(),
        .candidate_clause_ids = {},
        .active_num_vars = active_num_vars_,
    };
    assert(should_compact_before_decision(state) && "a substitution with no root unit");
    const std::vector<ClauseId> candidate_clause_ids =
        clause_database_.clause_ids_carrying(trail_.span().pivot_col_idxs());
    state.candidate_clause_ids = candidate_clause_ids;

    apply_substitution(build_substitution_plan(state));
    maybe_renumber();
}

void Solver::apply_substitution(SubstitutionPlan plan) {
    // Every rewrite line comes before any deletion: a one-row clause's line can be a
    // root unit's line, which the rewrite lines cite.
    std::vector<ClauseId> replaced_lines;

    // A clause is looked for among the clauses as they will stand, so every
    // rewritten one leaves the table before the first is looked for.
    for (const ClauseId clause_id : plan.rewritten_clause_ids) {
        equivalent_clauses_.remove(clause_id);
    }
    for (std::size_t i = 0; i < plan.rewritten_clause_ids.size(); ++i) {
        const ClauseId clause_id = plan.rewritten_clause_ids[i];
        assert(clause_database_.is_clause_active(clause_id) &&
               "the plan rewrote an inactive clause");
        std::optional<ClauseId> kept_id;
        if (plan.rewritten_is_active[i] != 0) {
            kept_id = equivalent_clauses_.find_or_enter(
                plan.rewritten_clauses[i],
                plan.rewritten_clauses[i].rows_hash(active_var_to_original_), clause_id,
                clause_database_.clauses());
        }
        if (kept_id.has_value()) {
            ++stats_.num_equivalent_clauses;
            redirect_clause_state(clause_id, *kept_id);
            evicted_clause_ids_.push_back(clause_id);
            plan.rewritten_is_active[i] = 0;
            plan.rewritten_clauses[i] = inactive_clause_placeholder(repr_, false_equation_);
        }
        if (proof_writer_ != nullptr && !kept_id.has_value()) {
            replaced_lines.push_back(proof_id_of_clause_[clause_id]);
            if (plan.rewritten_is_active[i] != 0) {
                proof_id_of_clause_[clause_id] = write_substitution_line(
                    clause_database_.clause(clause_id), plan.rewritten_clauses[i],
                    proof_id_of_clause_[clause_id]);
            }
        }
        if (plan.rewritten_is_active[i] == 0) {
            clause_database_.deactivate_clause(clause_id);
            selector_->update_with_inactive_clause(clause_id);
            --num_active_clauses_;
        }
        clause_database_.replace_clause(clause_id, std::move(plan.rewritten_clauses[i]));
        if (implication_graph_.has_value()) {
            implication_graph_->clause_was_rewritten(clause_id);
        }
    }
#ifndef NDEBUG
    // A clause the root trail satisfied carries one of its pivots, so no record
    // of that outlives the rewrite.
    for (ClauseId clause_id = 0; clause_id < clause_database_.num_clauses(); ++clause_id) {
        const Clause& clause = clause_database_.clause(clause_id);
        const ClauseRecord& record = clause_database_.record(clause_id);
        assert((!clause_database_.is_clause_active(clause_id) || record.satisfied_stamp == 0 ||
                record.satisfied_level != 0) &&
               "a clause the substitution left alone is recorded satisfied at the root");
        assert((!clause_database_.is_clause_active(clause_id) || debug_rows_are_reduced(clause)) &&
               "a clause is not stored in reduced row-echelon form");
    }
#endif

    if (proof_writer_ != nullptr) {
        for (const ClauseId line : proof_ids(evicted_clause_ids_)) {
            replaced_lines.push_back(line);
        }
        // No line cites a root unit after its substitution.
        replaced_lines.insert(replaced_lines.end(), proof_unit_only_lines_.begin(),
                              proof_unit_only_lines_.end());
        proof_unit_only_lines_.clear();
        if (!replaced_lines.empty()) {
            proof_writer_->write_deleted(replaced_lines);
        }
        assert(proof_root_unit_ids_.size() == trail_.rows_at_level(0) &&
               "a root trail row was never recorded as a unit");
        proof_root_unit_ids_.clear();
        proof_root_span_ = RowSpace(repr_, original_num_vars_);
    }
    evicted_clause_ids_.clear();
    assert(debug_equivalent_clause_table_is_exact() &&
           "the equivalent clause table is out of step with the active clauses");
    assert(debug_no_two_clauses_share_a_span() && "two active clauses have one span");

    // The root trail does not span `0 = 1`, so each of its rows drops a variable.
    dropped_col_idxs_.or_with(trail_.span().pivot_col_idxs());
    num_dropped_vars_ += trail_.span().rows();
    stats_.num_dropped_vars += trail_.span().rows();
    stats_.num_substituted_clauses += plan.rewritten_clause_ids.size();
    compaction_history_.push_back(std::move(plan.history_entry));

    // Its arcs are backed by clauses that may since have been rewritten.
    if (implication_graph_.has_value()) {
        implication_graph_->reconfigure(active_num_vars_);
    }

    // The root trail is thrown away on purpose; see the warning on this method.
    const Trail root_trail = std::exchange(trail_, Trail(repr_, active_num_vars_));
    did_complete_sweep_ = false;
    level_stamp_.assign(1, 1);
    decision_clause_of_level_.clear();

    selector_->update_with_substitution(plan.rewritten_clause_ids, dropped_col_idxs_, trail_);
    decisions_->update_with_substitution(root_trail.span(), dropped_col_idxs_,
                                         clause_database_.clauses(),
                                         clause_database_.active_flags());
    if (replay_.is_pending()) {
        replay_.decisions.substitute(root_trail.span());
    }
    ++stats_.num_compactions;
}

void Solver::maybe_renumber() {
    if (renumber_ && renumbering_frees_enough_words(active_num_vars_, num_dropped_vars_)) {
        apply_renumbering();
    }
}

void Solver::apply_renumbering() {
    assert(trail_.span().rows() == 0 && "a renumbering does not follow a substitution");
    const SolverPhase outer = phase_timer_.switch_to(SolverPhase::substitution);
    const CompactionTransform transform =
        CompactionTransform::from_dropped_columns(active_num_vars_, dropped_col_idxs_);
    assert(transform.new_num_vars() + num_dropped_vars_ == active_num_vars_ &&
           "the dropped variables were miscounted");

    // Composed through the numbering being retired, not replaced.
    const std::size_t new_num_vars = transform.new_num_vars();
    std::vector<std::size_t> renumbered_var_to_original(new_num_vars);
    for (std::size_t active_var = 0; active_var < new_num_vars; ++active_var) {
        renumbered_var_to_original[active_var] =
            active_var_to_original_[transform.to_old(active_var)];
    }

    active_num_vars_ = new_num_vars;
    active_var_to_original_ = std::move(renumbered_var_to_original);
    dropped_col_idxs_.resize(active_num_vars_ + 1);
    num_dropped_vars_ = 0;

    propagator_.reconfigure(active_num_vars_);
    if (implication_graph_.has_value()) {
        implication_graph_->reconfigure(active_num_vars_);
    }
    false_equation_ = Equation::eq_false(repr_, active_num_vars_);
    propagated_row_eqn_.resize(active_num_vars_);
    satisfied_probe_eqn_.resize(active_num_vars_);
    replay_implied_eqn_.resize(active_num_vars_);
    replay_reduced_eqn_.resize(active_num_vars_);
    trail_ = Trail(repr_, active_num_vars_);

    clause_database_.renumber(transform);
    selector_->update_with_renumbering(transform, trail_);
    decisions_->update_with_renumbering(transform);
    if (replay_.is_pending()) {
        replay_.decisions.renumber(transform);
    }
    compaction_history_.back().renumbering = transform;
    ++stats_.num_renumberings;
    phase_timer_.switch_to(outer);
}

void Solver::bump_responsible_clauses(const std::vector<std::size_t>& responsible_trail_idxs) {
    for (const std::size_t trail_idx : responsible_trail_idxs) {
        assert(trail_idx < trail_.reasons().size() && "a pivot names no trail row");
        const std::optional<ClauseId>& reason = trail_.reasons()[trail_idx];
        const std::optional<ClauseId> id =
            reason.has_value() ? reason : decision_clause_at(trail_idx);
        if (id.has_value()) {
            decisions_->update_with_resolved_clause(*id);
        }
    }
}

std::optional<ClauseId> Solver::decision_clause_at(std::size_t trail_idx) const {
    // A level opens with its decision, so that decision sits where the level
    // below it ended.
    const std::size_t level = trail_.level_of(trail_idx);
    assert(level > 0 && trail_.rows_at_level(level - 1) == trail_idx &&
           "no decision level opens at this trail row");
    return decision_clause_of_level_[level - 1];
}

void Solver::maybe_delete_clauses() {
    if (!deletion_.has_value()) {
        return;
    }
    if (stats_.num_conflicts - conflicts_at_last_deletion_ <
        conflicts_between_deletion_passes(*deletion_, stats_.num_conflicts)) {
        return;
    }
    conflicts_at_last_deletion_ = stats_.num_conflicts;
    phase_timer_.switch_to(SolverPhase::deletion);
    apply_deletion(build_deletion_plan(DeletionState{.database = clause_database_,
                                                     .trail = trail_,
                                                     .config = *deletion_,
                                                     .activity = *decisions_}));
}

void Solver::apply_deletion(const DeletionPlan& plan) {
    assert(plan.unused_passes.size() == num_active_clauses_ &&
           "the deletion plan does not cover the active clauses");

    std::size_t num_planned = 0;
    for (const ClauseId clause_id : clause_database_.active_clause_ids()) {
        ClauseRecord& record = clause_database_.record(clause_id);
        record.unused_passes = plan.unused_passes[num_planned++];
        record.used_since_deletion = false;
    }

    if (proof_writer_ != nullptr && !plan.deleted.empty()) {
        proof_writer_->write_deleted(proof_ids(plan.deleted));
    }
    for (const ClauseId clause_id : plan.deleted) {
        equivalent_clauses_.remove(clause_id);
        deactivate_clause(clause_id);
    }

    ++stats_.num_deletion_passes;
    stats_.num_deleted_clauses += plan.deleted.size();
}

bool Solver::implication_graph_is_due() {
    if (!implication_graph_config_.has_value()) {
        return false;
    }
    if (!implication_graph_run_is_owed_ &&
        stats_.num_conflicts - conflicts_at_last_implication_graph_point_ <
            implication_graph_config_->cadence) {
        return false;
    }
    const std::size_t num_new = clause_database_.num_clauses_reaching_width_2() -
                                width_2_clauses_at_last_implication_graph_run_;
    if (num_new >= implication_graph_config_->min_new_clauses) {
        return true;
    }
    count_implication_graph_point();
    ++stats_.implication_graph_skipped_runs;
    implication_graph_run_is_owed_ = false;
    conflicts_at_last_implication_graph_point_ = stats_.num_conflicts;
    return false;
}

void Solver::count_implication_graph_point() {
    const std::size_t num_new = clause_database_.num_clauses_reaching_width_2() -
                                width_2_clauses_at_last_implication_graph_run_;
    stats_.implication_graph_new_width_2_clauses += num_new;
    std::size_t& points = num_new == 0    ? stats_.implication_graph_points_new_0
                          : num_new < 10  ? stats_.implication_graph_points_new_1_9
                          : num_new < 50  ? stats_.implication_graph_points_new_10_49
                          : num_new < 100 ? stats_.implication_graph_points_new_50_99
                                          : stats_.implication_graph_points_new_100;
    ++points;
}

void Solver::run_implication_graph() {
    assert(trail_.level() == 0 && trail_.span().rows() == 0 &&
           "the implication graph runs on a substituted root");
    phase_timer_.switch_to(SolverPhase::implication_graph);
    count_implication_graph_point();
    implication_graph_run_is_owed_ = false;
    conflicts_at_last_implication_graph_point_ = stats_.num_conflicts;
    width_2_clauses_at_last_implication_graph_run_ =
        clause_database_.num_clauses_reaching_width_2();

    const ImplicationGraphRun summary = implication_graph_->run(
        clause_database_.clauses(), clause_database_.active_clause_ids(),
        proof_writer_ != nullptr ? &implication_graph_proof_ : nullptr);
    const std::vector<Equation>& units = summary.units;

    ++stats_.implication_graph_runs;
    stats_.implication_graph_pairs_examined += summary.pairs_examined;
    if (units.empty()) {
        return;
    }

    std::vector<ClauseId> unit_lines;
    if (proof_writer_ != nullptr) {
        unit_lines = write_implication_graph_lines(implication_graph_proof_);
        assert(unit_lines.size() == units.size() && "a fed unit went without a proof line");
    }
    for (std::size_t i = 0; i < units.size(); ++i) {
        feed_unit(units[i], proof_writer_ != nullptr ? unit_lines[i] : 0);
    }
    stats_.implication_graph_units_fed += units.size();
}

bool Solver::run_bve() {
    assert(trail_.level() == 0 && trail_.span().rows() == 0 &&
           "BVE runs on an empty trail, after the substitution of the root units");
    assert(stats_.num_conflicts == 0 && "BVE runs before any clause is learned");
    phase_timer_.switch_to(SolverPhase::bve);

    BvePlan plan = build_bve_plan(clause_database_.clauses(), clause_database_.active_clause_ids(),
                                  active_num_vars_, repr_, *bve_config_, proof_writer_ != nullptr, bve_rng_);
    if (stats_.bve_passes++ == 0) {
        stats_.bve_clauses_before = plan.counters.clauses_before;
        stats_.bve_width_before = plan.counters.width_before;
    }
    stats_.bve_eliminated += plan.counters.num_eliminated;
    stats_.bve_clauses_after = plan.counters.clauses_after;
    stats_.bve_width_after = plan.counters.width_after;
    stats_.bve_gate_searches += plan.counters.num_gate_searches;
    stats_.bve_gates += plan.counters.num_gates;
    stats_.bve_root_equations += plan.counters.num_root_equations;
    stats_.bve_gate_time_s += plan.counters.gate_time_s;
    // Another BVE pass follows one that removed at least `success_rate` of the clauses it read.
    const std::size_t before = plan.counters.clauses_before;
    const std::size_t after = plan.counters.clauses_after;
    bve_is_due_ = after < before && static_cast<double>(before - after) >=
                                        bve_config_->success_rate * static_cast<double>(before);
    std::vector<ClauseId> resolvent_lines;
    if (proof_writer_ != nullptr) {
        resolvent_lines = write_bve_lines(plan);
    }
    if (plan.found_empty_clause) {
        return false;
    }
    if (plan.eliminations.empty() && plan.resolvents.empty()) {
        return true;
    }

    if (proof_writer_ != nullptr) {
        std::vector<ClauseId> deleted;
        for (std::size_t i = 0; i < plan.resolvents.size(); ++i) {
            if (!plan.resolvents[i].clause.has_value()) {
                deleted.push_back(resolvent_lines[i]);
            }
        }
        for (const ClauseId clause_id : plan.removed_ids) {
            deleted.push_back(proof_id_of_clause_[clause_id]);
        }
        if (!deleted.empty()) {
            proof_writer_->write_deleted(deleted);
        }
    }
    // A clause the pass left stays under its id. The resolvents take the
    // removed clauses' ids in ascending order, and past them new ones.
    std::vector<ClauseId> rewritten_clause_ids;
    for (const ClauseId clause_id : plan.removed_ids) {
        equivalent_clauses_.remove(clause_id);
    }
    std::size_t num_ids_taken = 0;
    for (std::size_t i = 0; i < plan.resolvents.size(); ++i) {
        std::optional<Clause>& resolvent = plan.resolvents[i].clause;
        if (!resolvent.has_value()) {
            continue;
        }
        resolvent->reduce_rows();
        // Entered unlooked for: the pass keeps no resolvent with the span of a
        // clause it read or of another resolvent.
        const std::size_t key = resolvent->rows_hash(active_var_to_original_);
        assert(!equivalent_clauses_.find(*resolvent, key, clause_database_.clauses()).has_value() &&
               "a resolvent has the span of a kept clause");
        if (num_ids_taken < plan.removed_ids.size()) {
            const ClauseId clause_id = plan.removed_ids[num_ids_taken++];
            equivalent_clauses_.enter(clause_id, key);
            clause_database_.replace_with_new_clause(clause_id, std::move(*resolvent));
            rewritten_clause_ids.push_back(clause_id);
            if (proof_writer_ != nullptr) {
                proof_id_of_clause_[clause_id] = resolvent_lines[i];
            }
        } else {
            equivalent_clauses_.enter(clause_database_.num_clauses(), key);
            decisions_->update_with_new_clause(*resolvent, clause_database_.num_clauses());
            add_starting_clause_to_database(std::move(*resolvent));
            ++num_active_clauses_;
            if (proof_writer_ != nullptr) {
                proof_id_of_clause_.push_back(resolvent_lines[i]);
            }
        }
    }
    for (; num_ids_taken < plan.removed_ids.size(); ++num_ids_taken) {
        deactivate_clause(plan.removed_ids[num_ids_taken]);
    }
    assert(debug_equivalent_clause_table_is_exact() &&
           "the equivalent clause table is out of step with the active clauses");

    if (plan.eliminations.empty()) {
        // Only root equations were added, and they are propagated and substituted next.
        return true;
    }

    // No clause carries a variable the pass took out of every clause, so its
    // column is dropped as a substitution's is.
    const std::size_t num_dropped_vars = plan.dropped_col_idxs.count();
    dropped_col_idxs_.or_with(plan.dropped_col_idxs);
    num_dropped_vars_ += num_dropped_vars;
    stats_.num_dropped_vars += num_dropped_vars;
    compaction_history_.push_back(
        CompactionHistoryEntry{.eliminations = std::move(plan.eliminations),
                               .renumbering = std::nullopt});
    if (implication_graph_.has_value()) {
        implication_graph_->reconfigure(active_num_vars_);
    }
    did_complete_sweep_ = false;
    ++stats_.num_compactions;

    selector_->update_with_substitution(rewritten_clause_ids, dropped_col_idxs_, trail_);
    decisions_->update_with_substitution(trail_.span(), dropped_col_idxs_,
                                         clause_database_.clauses(),
                                         clause_database_.active_flags());
    // A unit resolvent is propagated and substituted next, and that substitution renumbers.
    if (plan.counters.num_unit_resolvents == 0) {
        maybe_renumber();
    }
    return true;
}

std::vector<ClauseId> Solver::write_bve_lines(const BvePlan& plan) {
    assert(proof_writer_ != nullptr && "no proof to record the resolvents in");
    std::vector<ClauseId> lines;
    lines.reserve(plan.resolvents.size());
    std::vector<ProofRow> rows;
    std::vector<ClauseId> lemma_lines;
    for (const BvePlan::Resolvent& resolvent : plan.resolvents) {
        const auto line_of = [&](const BvePlan::Parent& parent) {
            switch (parent.kind) {
            case BvePlan::Parent::Kind::clause:
                return proof_id_of_clause_[parent.idx];
            case BvePlan::Parent::Kind::resolvent:
                return lines[parent.idx];
            case BvePlan::Parent::Kind::lemma:
                return lemma_lines[parent.idx];
            }
            return ClauseId{0};
        };
        const auto lines_of = [&](const std::vector<BvePlan::Parent>& parents) {
            std::vector<ClauseId> cited;
            cited.reserve(parents.size());
            for (const BvePlan::Parent& parent : parents) {
                cited.push_back(line_of(parent));
            }
            return cited;
        };
        // A root equation's lemmas are lines of their own, deleted once it is written.
        lemma_lines.clear();
        for (const BvePlan::Lemma& lemma : resolvent.lemmas) {
            const std::vector<ClauseId> reasons = lines_of(lemma.reasons);
            lemma_lines.push_back(next_proof_clause_id_++);
            proof_writer_->write_learned(lemma_lines.back(), lemma.clause, active_var_to_original_, reasons);
        }
        // Under a resolvent's linear negation the first parent propagates and the
        // second is falsified.
        const std::vector<ClauseId> reasons =
            resolvent.reasons.empty()
                ? std::vector<ClauseId>{line_of(resolvent.parents[0]), line_of(resolvent.parents[1])}
                : lines_of(resolvent.reasons);
        lines.push_back(next_proof_clause_id_++);
        if (!resolvent.clause.has_value()) {
            rows.clear();
            for (const VarListEquation& row : plan.eliminations[resolvent.elimination_idx]
                                                  .removed_clauses[resolvent.removed_clause_idx]) {
                rows.push_back(lift_row(row, active_var_to_original_, original_num_vars_));
            }
            proof_writer_->write_learned(lines.back(), rows, reasons);
        } else if (resolvent.clause->rows() == 0) {
            proof_writer_->write_empty(lines.back(), reasons);
            break;
        } else {
            proof_writer_->write_learned(lines.back(), *resolvent.clause, active_var_to_original_,
                                         reasons);
        }
        if (!lemma_lines.empty()) {
            proof_writer_->write_deleted(lemma_lines);
        }
    }
    return lines;
}

void Solver::feed_unit(const Equation& unit, const ClauseId proof_line) {
    assert(trail_.level() == 0 && "a unit is fed at the root");
    assert(unit.num_bits() == active_num_vars_ + 1 &&
           "a fed unit is not over the active variable numbering");
#ifndef NDEBUG
    Equation reduced_unit = unit;
    assert(trail_.span().partial_reduce(reduced_unit).kind != ReduceResult::Kind::eq_true &&
           "a fed unit is in the root span");
#endif

    Clause clause(repr_, active_num_vars_);
    Equation row = unit;
    row.negate();
    clause.add_row(std::move(row));
    assert(clause.rows() == 1 && "a fed unit did not make a width-1 clause");

    const ClauseId new_clause_id = clause_database_.num_clauses();
    if (proof_writer_ != nullptr) {
        assert(proof_id_of_clause_.size() == new_clause_id && "a clause went without a proof id");
        proof_id_of_clause_.push_back(proof_line);
    }
    decisions_->update_with_new_clause(clause, new_clause_id);
    // Entered unlooked for: the unit is not in the root span, which holds the
    // equation of every active width-1 clause once the root is propagated.
    const std::size_t key = clause.rows_hash(active_var_to_original_);
    assert(!equivalent_clauses_.find(clause, key, clause_database_.clauses()).has_value() &&
           "a fed unit has the span of a kept clause");
    add_asserting_clause_to_database(std::move(clause),
                                     AssertingClauseInfo{.lbd = 0, .removed_levels = 0});
    ++num_active_clauses_;
    equivalent_clauses_.enter(new_clause_id, key);
}

ClauseId Solver::write_proof_line(const Clause& clause, const std::vector<ClauseId>& hints) {
    if (proof_writer_ == nullptr) {
        return 0;
    }
    const ClauseId line = next_proof_clause_id_++;
    proof_writer_->write_learned(line, clause, active_var_to_original_, hints);
    return line;
}

void Solver::feed_units(const std::vector<std::pair<Equation, ClauseId>>& units, RowSpace fed) {
    for (const auto& [unit, proof_line] : units) {
        if (fed.add_row_get_row_idx(unit).has_value()) {
            feed_unit(unit, proof_line);
        }
    }
}

void Solver::remove_clause(const ClauseId id) {
    if (proof_writer_ != nullptr) {
        proof_writer_->write_deleted({proof_id_of_clause_[id]});
    }
    equivalent_clauses_.remove(id);
    deactivate_clause(id);
}

void Solver::deactivate_clause(const ClauseId id) {
    clause_database_.deactivate_clause(id);
    selector_->update_with_inactive_clause(id);
    clause_database_.replace_clause(id, inactive_clause_placeholder(repr_, false_equation_));
    --num_active_clauses_;
}

Solver::EvictedClauseState Solver::evicted_clause_state(const ClauseId id) const {
    const ClauseRecord& record = clause_database_.record(id);
    return EvictedClauseState{.is_learned = record.learned,
                              .lbd = record.lbd_measured ? std::optional(record.lbd)
                                                         : std::nullopt,
                              .unused_passes = record.unused_passes,
                              .is_used_since_deletion = record.used_since_deletion};
}

void Solver::redirect_clause_state(const ClauseId retired_id, const ClauseId kept_id) {
    merge_deletion_state(kept_id, evicted_clause_state(retired_id));
    decisions_->update_with_subsumed_clause(retired_id, kept_id);
    note_clause_kept_in_place_of(retired_id, kept_id);
}

void Solver::merge_deletion_state(const ClauseId kept_id, const EvictedClauseState& removed) {
    ClauseRecord& kept = clause_database_.record(kept_id);
    kept.take_lower_lbd(removed.lbd);
    if (!removed.is_learned) {
        // Standing for a starting clause, the kept clause is never deleted.
        kept.learned = false;
    } else if (kept.learned) {
        kept.unused_passes = std::min(kept.unused_passes, removed.unused_passes);
        if (removed.is_used_since_deletion) {
            kept.used_since_deletion = true;
        }
    }
}

#ifndef NDEBUG
bool Solver::debug_equivalent_clause_table_is_exact() const {
    if (equivalent_clauses_.size() != num_active_clauses_) {
        return false;
    }
    for (const ClauseId clause_id : clause_database_.active_clause_ids()) {
        if (!equivalent_clauses_.is_entered(clause_id)) {
            return false;
        }
    }
    return true;
}

bool Solver::debug_no_two_clauses_share_a_span() {
    // A clause learned short of a fixed point is kept unlooked for.
    if (budget_.has_value() || ++debug_span_checks_ % (1 + num_active_clauses_ / 256) != 0) {
        return true;
    }
    // Read off reductions made here, not off the rows as stored.
    std::vector<RowSpace> spans;
    std::vector<std::pair<std::size_t, std::size_t>> hash_to_span_idx;
    for (const ClauseId clause_id : clause_database_.active_clause_ids()) {
        const RowSpace& stored = clause_database_.clause(clause_id);
        RowSpace span = stored;
        span.reduce_rows();
        std::size_t hash = span.rows();
        for (const Equation& row : span) {
            hash = hash * 31 + row.hash();
        }
        hash_to_span_idx.emplace_back(hash, spans.size());
        spans.push_back(std::move(span));
    }
    std::ranges::sort(hash_to_span_idx);
    for (std::size_t i = 0; i < hash_to_span_idx.size(); ++i) {
        for (std::size_t j = i + 1;
             j < hash_to_span_idx.size() && hash_to_span_idx[j].first == hash_to_span_idx[i].first;
             ++j) {
            if (spans[hash_to_span_idx[i].second].has_rows_of(spans[hash_to_span_idx[j].second])) {
                return false;
            }
        }
    }
    return true;
}
#endif

Solution Solver::solve() {
    phase_timer_.start();
    Solution solution = search();
    phase_timer_.stop();
    return solution;
}

Solution Solver::search() {
#ifndef NDEBUG
    has_solved_ = true;
#endif
    assert(active_clause_count_is_consistent() &&
           "the starting clauses left the live-clause count wrong");

    if (empty_starting_clause_proof_id_) {
        if (proof_writer_ != nullptr) {
            proof_writer_->write_empty(next_proof_clause_id_++, {*empty_starting_clause_proof_id_});
        }
        return Solution{SolveResult::unsat, std::nullopt};
    }

    if (attempt_budget_config_.has_value()) {
        budget_.emplace(attempt_budget_config_->miss_rate, attempt_budget_config_->window,
                        std::max<std::size_t>(1, clause_database_.num_clauses()));
    }

    solve_start_ = std::chrono::steady_clock::now();
    if (modes_.has_value()) {
        decisions_->update_with_mode(modes_->mode());
    }
    if (reports_progress()) {
        report_progress();
    }

    while (true) {
        phase_timer_.switch_to(SolverPhase::propagation);
        if (propagate_until_conflict()) {
            if (const std::optional<Solution> solution = handle_conflict()) {
                return *solution;
            }
            continue;
        }

        // Quiescent at full rank: the trail is a total assignment.
        if (trail_.span().rows() == active_num_vars()) {
            if (did_complete_sweep_ || !last_run_stopped_on_budget()) {
                return complete_satisfying_assignment();
            }
            // Full rank reached on a budgeted run is not yet quiescence, and one
            // unbudgeted run settles it. The flag is what keeps that from
            // repeating.
            did_complete_sweep_ = true;
            force_complete_sweep_ = true;
            continue;
        }

        if (trail_.level() == 0) {
            const VisitStep step = visit_root_step();
            if (step == VisitStep::unsat) {
                return Solution{SolveResult::unsat, std::nullopt};
            }
            if (step == VisitStep::again) {
                continue;
            }
        } else if (!replay_.is_pending() && forced_restart_is_due()) {
            force_restart();
            continue;
        }

        switch (replay_.is_pending() ? replay_decision_step() : make_decision_step()) {
            case DecisionStep::decided:
                break;
            case DecisionStep::conflicting:
                if (const std::optional<Solution> solution = handle_conflict()) {
                    return *solution;
                }
                break;
            case DecisionStep::propagating: {
                // Unit or falsified modulo the trail, so the watch logic has an
                // equation to return whichever pair of rows it is watching.
                phase_timer_.switch_to(SolverPhase::propagation);
                const PropagationOutcome outcome =
                    propagate_from_clause(propagating_clause_id_);
                assert(outcome != PropagationOutcome::nothing &&
                       "a clause the decision step handed back propagated nothing");
                if (outcome == PropagationOutcome::conflicting) {
                    if (const std::optional<Solution> solution = handle_conflict()) {
                        return *solution;
                    }
                }
                break;
            }
            case DecisionStep::exhausted:
                return complete_satisfying_assignment();
        }
    }
}

Solution Solver::complete_satisfying_assignment() {
    phase_timer_.switch_to(SolverPhase::other);
    // Every active clause is satisfied, so any completion of the trail is a
    // model.
    RowSpace complete = trail_.span();
    Equation unit(repr_, active_num_vars_);
    Equation reduced(repr_, active_num_vars_);
    std::size_t num_filled = 0;
    for (std::size_t var = 0; var < active_num_vars_; ++var) {
        unit.assign_bits(std::span<const std::size_t>(&var, 1));
        reduced.assign_from(unit);
        // Undetermined exactly when a variable still leads: `0 = 0` means the
        // trail already fixes it one way, `0 = 1` the other. A dropped variable
        // leads too, and takes its value from the compaction history instead.
        if (complete.partial_reduce(reduced).kind == ReduceResult::Kind::leading) {
            complete.add_row(unit);
            num_filled += dropped_col_idxs_.test(var) ? 0U : 1U;
        }
    }
    decisions_->note_filled_decisions(num_filled);
    assert(complete.rows() == active_num_vars_ && "the completed trail is not a total assignment");
    assert(every_active_clause_is_satisfied(complete) &&
           "the model reported as satisfying falsifies an active clause");

    IndexSet original_assignment = reconstruct_original_assignment(
        complete.unique_solution(), compaction_history_, used_vars_, original_num_vars_);
    return Solution{SolveResult::sat, std::move(original_assignment)};
}

void Solver::switch_mode() {
    decisions_->update_with_mode(modes_->mode());
}

RunStats Solver::stats() const {
    RunStats stats = stats_;
    stats.num_propagations = propagation_counters_.num_propagations;
    stats.num_attempt_props = propagation_counters_.num_attempt_props;
    stats.num_watch_updates = propagation_counters_.num_watch_updates;
    stats.num_out_of_order_learns = propagation_counters_.num_out_of_order_learns;
    stats.num_vars = num_vars();
    stats.unused_vars = original_num_vars_ - used_vars_.size();
    stats.active_vars = active_num_vars();
    stats.clauses_in_database = num_active_clauses();
    stats.average_learned_lbd = num_asserting_clauses_ == 0
                                    ? 0.0
                                    : static_cast<double>(sum_learned_lbd_) /
                                          static_cast<double>(num_asserting_clauses_);
    stats.decision_time_s = phase_timer_.seconds(SolverPhase::decisions);
    stats.propagation_time_s = phase_timer_.seconds(SolverPhase::propagation);
    stats.analysis_time_s = phase_timer_.seconds(SolverPhase::analysis);
    stats.deletion_time_s = phase_timer_.seconds(SolverPhase::deletion);
    stats.substitution_time_s = phase_timer_.seconds(SolverPhase::substitution);
    stats.bve_time_s = phase_timer_.seconds(SolverPhase::bve);
    stats.subsume_pass_time_s = phase_timer_.seconds(SolverPhase::subsume_pass);
    stats.vivify_time_s = phase_timer_.seconds(SolverPhase::vivification);
    stats.implication_graph_time_s = phase_timer_.seconds(SolverPhase::implication_graph);
    stats.replay_time_s = phase_timer_.seconds(SolverPhase::replay);
    stats.other_time_s = phase_timer_.seconds(SolverPhase::other);
    decisions_->add_to_stats(stats);
    restarts_->add_to_stats(stats);
    if (modes_.has_value()) {
        stats.focused_canceled_restarts = focused_restarts_->num_canceled();
        stats.focused_fast_lbd = focused_restarts_->fast_lbd();
        stats.focused_slow_lbd = focused_restarts_->slow_lbd();
    }
    return stats;
}

} // namespace xorcle
