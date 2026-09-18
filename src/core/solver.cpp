#include "core/solver.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <chrono>
#include <numeric>
#include <optional>
#include <span>
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
               const DeletionConfig& deletion, bool bump_responsible_clauses, bool subsume)
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
      proof_root_span_(repr, num_vars),
      conflict_check_eqn_(repr, num_vars),
      false_equation_(Equation::eq_false(repr, num_vars)),
      progress_(progress),
      minimize_(minimize),
      bump_responsible_clauses_(bump_responsible_clauses),
      subsume_(subsume) {
    assert(selector_ != nullptr && "the clause selector must not be null");
    assert(decisions_ != nullptr && "the decision procedure must not be null");
    assert(restarts_ != nullptr && "the restart policy must not be null; use NoRestarts");
    assert((proof_writer_ == nullptr || proof_writer_->num_vars() == original_num_vars_) &&
           "the proof writer's variable numbering must be the instance's");
    assert((!deletion_.enabled || deletion_.key != DeletionKey::activity ||
            deletion_.activity != nullptr) &&
           "the activity key has no activity source");
    // The run's seed lives on the decision procedure, so this waits for its null check.
    representative_rng_ = make_rng(decisions_->base_seed(), RngComponent::minimize_representative);
    std::iota(active_var_to_original_.begin(), active_var_to_original_.end(), std::size_t{0});
    if (proof_writer_ != nullptr) {
        proof_col_idx_to_unit_idx_.resize(num_vars);
    }
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
    ++num_active_clauses_;
    if (proof_writer_ != nullptr) {
        proof_id_of_clause_.push_back(next_proof_clause_id_++);
    }
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
        if (proof_writer_ != nullptr && trail_.level() == 0) {
            write_root_unit(selection->id);
        }
    }
}

void Solver::handle_non_root_conflict() {
    AssertingClause learned =
        find_asserting_clause(trail_, selector_->clauses(), *decisions_, representative_rng_,
                              minimize_, deletion_.enabled, bump_responsible_clauses_,
                              subsume_);
    if (minimize_) {
        counters_.minimize_width_before += learned.width_before_minimize;
        counters_.minimize_width_after += learned.clause.rows();
        counters_.num_minimized_clauses +=
            learned.clause.rows() < learned.width_before_minimize ? 1 : 0;
    }
    // Density of the learned clause in variable space, not just its row count.
    for (const Equation& row : learned.clause) {
        counters_.learned_equation_bits += row.num_set_bits();
    }
    if (deletion_.enabled) {
        // Before the proof path reverses `reason_ids`, which `reason_lbds` parallels.
        for (std::size_t i = 0; i < learned.reason_ids.size(); ++i) {
            note_reason_use(learned.reason_ids[i], learned.reason_lbds[i]);
        }
    }
    // With the resolved-clause bumps, before the decay below.
    if (bump_responsible_clauses_) {
        bump_responsible_clauses(learned.responsible_trail_idxs);
    }

    // The id the clause is *about* to take, read before it is added.
    const ClauseId new_clause_id = selector_->num_clauses();

    // Records the activity the new clause will enter at, and decays. Must precede
    // the `update_with_new_clause()` below, which spends that recorded activity.
    decisions_->update_with_asserting_clause(learned.clause);

    assert((learned.clause.rows() != 1 || learned.asserting_level == 0) &&
           "a unit asserting clause must assert at the root");
    const std::size_t level_before_backjump = trail_.level();
    // Sampled at the conflict rather than after the backjump, which is where the
    // progress line used to read them and why they read low.
    counters_.sum_learned_lbd += learned.lbd;
    ++counters_.num_asserting_clauses;
    counters_.sum_conflict_level += level_before_backjump;
    counters_.sum_conflict_trail_length += trail_.span().rows();
    // Consumes a conflict from the restart budget, so it must run exactly once
    // per non-root conflict whether or not a restart fires.
    const bool restart_now = restarts_->should_restart_after_conflict(
        ConflictSummary{.lbd = learned.lbd, .trail_length = trail_.span().rows()});

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
                                             learned.reason_ids.rend() - prefix,
                                             learned.reason_ids.rend());
            }
        }
    }

    if (proof_writer_ != nullptr) {
        // Derivation order to replay order. Exactly one reversal, here.
        std::reverse(learned.reason_ids.begin(), learned.reason_ids.end());
        assert(proof_id_of_clause_.size() == new_clause_id &&
               "a clause went without a proof id");
        proof_id_of_clause_.push_back(next_proof_clause_id_++);
        proof_writer_->write_learned(proof_id_of_clause_.back(), learned.clause,
                                     active_var_to_original_, proof_ids(learned.reason_ids));
    }

    const AssertingClauseInfo learned_info{.lbd = learned.lbd, .removed_levels = removed_levels};
    selector_->add_asserting_clause(std::move(learned.clause), trail_, learned_info);
    ++num_active_clauses_;

    // A reason a trail row still names is left as is, replacement included:
    // that case never occurred in any measured run, so acting on it would ship
    // behavior nothing has exercised.
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
        assert(selector_->is_clause_active(record.clause_id) &&
               "a conflict reason must be active");
        if (reason_still_named(record)) {
            continue;
        }
        // A replacement takes the subsumed clause's standing, and so on along
        // chains of replacements: what replaces an input clause is no more an
        // deletion candidate than the input clause was, and what replaces a core
        // clause stays in core.
        const bool replaces_input = !selector_->is_clause_learned(record.clause_id);
        const bool replaces_core = selector_->clause_tier(record.clause_id) == ClauseTier::core;
        ClauseId replacement_id = new_clause_id;
        if (record.replacement.has_value()) {
            replacement_id = selector_->num_clauses();
            decisions_->update_with_new_clause(*record.replacement, replacement_id);
            if (proof_writer_ != nullptr) {
                proof_id_of_clause_.push_back(next_proof_clause_id_++);
                proof_writer_->write_learned(proof_id_of_clause_.back(), *record.replacement,
                                             active_var_to_original_,
                                             proof_ids(replacement_chains[i]));
            }
            selector_->add_asserting_clause(
                std::move(*record.replacement), trail_,
                AssertingClauseInfo{.lbd = record.lbd,
                                   .removed_levels = removed_levels,
                                   .is_replacement = true});
            ++num_active_clauses_;
            ++counters_.num_subsume_replacements;
        }
        if (replaces_input) {
            selector_->set_clause_learned(replacement_id, false);
        } else if (replaces_core) {
            selector_->set_clause_tier(replacement_id, ClauseTier::core);
        }
        decisions_->update_with_subsumed_clause(record.clause_id, replacement_id);
    }
    for (const SubsumedReason& record : learned.subsumed_reasons) {
        if (reason_still_named(record)) {
            continue;
        }
        if (proof_writer_ != nullptr) {
            proof_writer_->write_deleted({proof_id_of_clause_[record.clause_id]});
        }
        selector_->clause(record.clause_id) = inactive_clause_placeholder(repr_, false_equation_);
        selector_->set_clause_active(record.clause_id, false);
        --num_active_clauses_;
        ++counters_.num_subsumed_clauses;
        counters_.num_subsumed_conflicting_clauses += record.subsumed_conflicting_clause ? 1 : 0;
    }
}

void Solver::write_root_unsat_proof() {
    if (proof_writer_ == nullptr) {
        return;
    }
    proof_writer_->write_empty(next_proof_clause_id_++,
                               proof_ids(find_root_conflict_reasons(trail_, selector_->clauses(),
                                                                    representative_rng_)));
}

void Solver::write_root_unit(ClauseId reason_id) {
    assert(proof_writer_ != nullptr && "no proof to record a root unit in");
    assert(trail_.level() == 0 && "a root unit is propagated at the root");

    // Reduced by the units before it, the row is what the compaction record
    // holds, so a pivot there names exactly this line.
    const RawRow trail_row = lift_row(trail_.span().row(trail_.trail_idx_to_row_idx().back()),
                                      active_var_to_original_, original_num_vars_);
    Equation unit = Equation::from_bits(repr_, original_num_vars_, trail_row);
    proof_root_span_.reduce(unit);
    Equation negated_unit = unit;
    negated_unit.plus_assign(Equation::eq_false(repr_, original_num_vars_));

    // Every reason row reduces to `0 = 0` or to the unit's linear negation, so
    // the units those reductions used are all the line needs before its reason.
    const Clause& reason = selector_->clause(reason_id);
    IndexSet used(proof_root_span_.rows());
    Equation reason_row(repr_, original_num_vars_);
    for (const Equation& row : reason) {
        reason_row.assign_bits(lift_row(row, active_var_to_original_, original_num_vars_));
        const IndexSet coefficients = proof_root_span_.reduce_get_coefficients(reason_row);
        assert((reason_row.is_eq_true() || reason_row == negated_unit) &&
               "the reason clause does not propagate the root unit");
        coefficients.for_each_set([&used](std::size_t row_idx) { used.set(row_idx); });
    }

    // A one-row reason needing no unit already is the line, unless a compaction
    // has rewritten it since it was written.
    ClauseId unit_id = proof_id_of_clause_[reason_id];
    if (reason.rows() != 1 || !used.none() || unit_id < first_proof_id_of_epoch_) {
        std::vector<ClauseId> reasons;
        used.for_each_set([this, &reasons](std::size_t row_idx) {
            reasons.push_back(
                proof_col_idx_to_unit_idx_[*proof_root_span_.row(row_idx).leading_bit()]);
        });
        reasons.push_back(unit_id);
        unit_id = next_proof_clause_id_++;
        const RawRow negated_row = negated_unit.bits();
        proof_writer_->write_learned(unit_id, std::span<const RawRow>(&negated_row, 1), reasons);
    }

    RawRow unit_row = unit.bits();
    const std::optional<std::size_t> row_idx =
        proof_root_span_.add_row_get_row_idx(std::move(unit));
    assert(row_idx.has_value() && "a root unit is in the span of the ones before it");
    proof_col_idx_to_unit_idx_[*proof_root_span_.row(*row_idx).leading_bit()] = unit_id;
    proof_root_units_.push_back(RawUnit{std::move(unit_row), unit_id});
}

void Solver::write_compaction_units() {
    assert(proof_writer_ != nullptr && "no proof to record a compaction in");
    assert(proof_root_units_.size() == trail_[0].span().rows() &&
           "a root trail row was never recorded as a unit");
    proof_writer_->write_compaction(proof_root_units_);
    proof_root_units_.clear();
    proof_root_span_ = RowSpace(repr_, original_num_vars_);
    first_proof_id_of_epoch_ = next_proof_clause_id_;
}

std::vector<ClauseId> Solver::proof_ids(const std::vector<ClauseId>& clause_ids) const {
    std::vector<ClauseId> ids;
    ids.reserve(clause_ids.size());
    for (const ClauseId id : clause_ids) {
        ids.push_back(proof_id_of_clause_[id]);
    }
    return ids;
}

void Solver::backjump_to(std::size_t level) {
    assert(level <= trail_.level() && "backjump target is above the current level");
    trail_.resize(level + 1);
    decision_clause_of_level_.resize(level);
}

bool Solver::make_decision_step() {
    std::optional<Equation> decision =
        decisions_->make_decision(trail_.span(), selector_->clauses(), selector_->active_flags());
    if (!decision.has_value()) {
        return false;
    }
    Equation decision_eqn = *std::move(decision);
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
    return true;
}

#ifndef NDEBUG
bool Solver::active_clause_count_is_consistent() const {
    const std::vector<std::uint8_t>& active = selector_->active_flags();
    return num_active_clauses_ ==
           static_cast<std::size_t>(std::count(active.begin(), active.end(), 1));
}
#endif

void Solver::maybe_report() {
    if (!reports_progress()) {
        return;
    }
    if (counters_.num_conflicts - conflicts_at_last_report_ < progress_.cadence) {
        return;
    }
    report_progress();
}

void Solver::report_progress() {
    const ProgressSnapshot snapshot = progress_snapshot();
    progress_.reporter->report(snapshot);
    conflicts_at_last_report_ = counters_.num_conflicts;
    sum_level_at_last_report_ = counters_.sum_conflict_level;
    sum_trail_length_at_last_report_ = counters_.sum_conflict_trail_length;
    time_at_last_report_s_ = snapshot.time_s;
}

ProgressSnapshot Solver::progress_snapshot() const {
    assert(active_clause_count_is_consistent() &&
           "the tracked live-clause count fell out of step with the active flags");
    const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - solve_start_;
    const double time_s = elapsed.count();

    // Everything below is taken over the interval since the last line, so a
    // reader sees the current trend rather than the run's flattening average.
    const std::size_t interval_conflicts = counters_.num_conflicts - conflicts_at_last_report_;
    const double interval_s = time_s - time_at_last_report_s_;
    const auto per_conflict = [interval_conflicts](const std::size_t interval_sum) {
        return interval_conflicts == 0 ? 0.0
                                       : static_cast<double>(interval_sum) /
                                             static_cast<double>(interval_conflicts);
    };

    return ProgressSnapshot{
        .time_s = time_s,
        .num_conflicts = counters_.num_conflicts,
        .conflicts_per_second =
            interval_s > 0.0 ? static_cast<double>(interval_conflicts) / interval_s : 0.0,
        .num_propagations = counters_.num_propagations,
        .num_clauses = num_active_clauses_,
        .average_decision_level =
            per_conflict(counters_.sum_conflict_level - sum_level_at_last_report_),
        .average_trail_length = per_conflict(counters_.sum_conflict_trail_length -
                                            sum_trail_length_at_last_report_),
        .remaining_vars = active_num_vars_ - trail_[0].span().rows(),
        .decisions = decisions_->decision_stats().decisions,
        .decision_level = trail_.level(),
        .trail_length = trail_.span().rows(),
        .num_restarts = counters_.num_restarts,
        .num_reused_levels = counters_.num_reused_levels,
        .num_deleted_clauses = counters_.num_deleted_clauses,
        .num_demoted_clauses = counters_.num_demoted_clauses,
        .num_promoted_clauses = counters_.num_promoted_clauses,
        .num_attempt_props = counters_.num_attempt_props,
        .num_watch_updates = counters_.num_watch_updates};
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
        .cadence = compaction_.cadence,
    };
    if (!should_compact_before_decision(state)) {
        return false;
    }

    const CompactionTransform transform = build_compaction_transform(trail_, active_num_vars_);
    assert(transform.new_num_vars() < active_num_vars_ &&
           "the compaction trigger held but nothing can be dropped");

    apply_compaction(build_compaction_plan(state, transform));
    return true;
}

void Solver::apply_compaction(CompactionPlan plan) {
    // The root trail is about to go, and the proof needs to know its units.
    if (proof_writer_ != nullptr) {
        write_compaction_units();
    }

    const std::size_t num_clauses = selector_->num_clauses();
    assert(plan.rewritten_clauses.size() == num_clauses);
    assert(plan.clause_is_active.size() == num_clauses);

    num_active_clauses_ = 0;
    for (ClauseId clause_id = 0; clause_id < num_clauses; ++clause_id) {
        selector_->clause(clause_id) = std::move(plan.rewritten_clauses[clause_id]);
        const bool active = plan.clause_is_active[clause_id] != 0;
        selector_->set_clause_active(clause_id, active);
        num_active_clauses_ += active ? 1 : 0;
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

    // The root trail is thrown away on purpose; see the warning on this method.
    trail_ = Trail(repr_, active_num_vars_);
    decision_clause_of_level_.clear();

    selector_->update_with_compaction(plan.transform, trail_);
    decisions_->update_with_compaction(plan.transform, trail_.span(), selector_->clauses(),
                                       selector_->active_flags());
    has_compacted_once_ = true;
    conflicts_at_last_compaction_ = counters_.num_conflicts;
    ++counters_.num_compactions;
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
        selector_->set_clause_unused_passes(id, 0);
        ++counters_.num_promoted_clauses;
    }
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
    for (std::size_t level = 1; level <= trail_.level(); ++level) {
        if (trail_[level - 1].span().rows() == trail_idx) {
            return decision_clause_of_level_[level - 1];
        }
    }
    assert(false && "no decision level opens at this trail row");
    return std::nullopt;
}

void Solver::maybe_delete_clauses() {
    if (!deletion_.enabled) {
        return;
    }
    if (counters_.num_conflicts - conflicts_at_last_deletion_ < deletion_.cadence) {
        return;
    }
    conflicts_at_last_deletion_ = counters_.num_conflicts;
    apply_deletion(build_deletion_plan(DeletionState{.selector = *selector_,
                                                     .trail = trail_,
                                                     .demote_cadence_in_passes =
                                                         deletion_.demote_cadence_in_passes,
                                                     .key = deletion_.key,
                                                     .activity = deletion_.activity}));
}

void Solver::apply_deletion(const DeletionPlan& plan) {
    const std::size_t num_clauses = selector_->num_clauses();
    assert(plan.unused_passes.size() == num_clauses);

    for (ClauseId clause_id = 0; clause_id < num_clauses; ++clause_id) {
        selector_->set_clause_unused_passes(clause_id, plan.unused_passes[clause_id]);
        selector_->set_clause_used_since_deletion(clause_id, false);
    }
    for (const ClauseId clause_id : plan.demoted) {
        selector_->set_clause_tier(clause_id, ClauseTier::local);
    }

    if (proof_writer_ != nullptr && !plan.deleted.empty()) {
        proof_writer_->write_deleted(proof_ids(plan.deleted));
    }
    for (const ClauseId clause_id : plan.deleted) {
        selector_->clause(clause_id) = inactive_clause_placeholder(repr_, false_equation_);
        selector_->set_clause_active(clause_id, false);
    }

    ++counters_.num_deletion_passes;
    num_active_clauses_ -= plan.deleted.size();
    counters_.num_deleted_clauses += plan.deleted.size();
    counters_.num_demoted_clauses += plan.demoted.size();
}

Solution Solver::solve() {
#ifndef NDEBUG
    has_solved_ = true;
#endif
    assert(active_clause_count_is_consistent() &&
           "the starting clauses left the live-clause count wrong");

    solve_start_ = std::chrono::steady_clock::now();
    if (reports_progress()) {
        report_progress();
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
            return complete_satisfying_assignment();
        }

        if (compaction_.enabled && maybe_compact_before_decision()) {
            // The active numbering changed, so re-propagate rather than decide.
            continue;
        }

        if (!make_decision_step()) {
            return complete_satisfying_assignment();
        }
    }
}

Solution Solver::complete_satisfying_assignment() {
    // Every active clause is satisfied, so any completion of the trail is a
    // model. Filling the rest here rather than deciding them one at a time
    // spares a propagation sweep and a trail snapshot per variable.
    RowSpace complete = trail_.span();
    Equation unit(repr_, active_num_vars_);
    Equation reduced(repr_, active_num_vars_);
    std::size_t num_filled = 0;
    for (std::size_t var = 0; var < active_num_vars_; ++var) {
        unit.assign_bits(std::span<const std::size_t>(&var, 1));
        reduced.assign_from(unit);
        // Undetermined exactly when a variable still leads: `0 = 0` means the
        // trail already fixes it one way, `0 = 1` the other.
        if (complete.partial_reduce(reduced).kind == ReduceResult::Kind::leading) {
            complete.add_row(unit);
            ++num_filled;
        }
    }
    decisions_->note_filled_decisions(num_filled);
    assert(complete.rows() == active_num_vars_ && "the completed trail is not a total assignment");

    IndexSet original_assignment = reconstruct_original_assignment(
        complete.unique_solution(), compaction_history_, active_var_to_original_,
        original_num_vars_);
    return Solution{SolveResult::sat, std::move(original_assignment)};
}

} // namespace xorcle
