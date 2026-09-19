#include "core/solver.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <chrono>
#include <numeric>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "algebra/reduce_result.h"
#include "core/analysis.h"
#include "core/trail_basis.h"
#include "heuristics/compaction/original_variables.h"

namespace xorcle {

Solver::Solver(EquationRepr repr, std::size_t num_vars, std::unique_ptr<ClauseSelector> selector,
               std::unique_ptr<DecisionProcedure> decisions,
               std::unique_ptr<RestartPolicy> restarts, RawProofWriter* proof_writer,
               const CompactionConfig& compaction, const ProgressConfig& progress, bool minimize,
               const DeletionConfig& deletion, bool bump_responsible_clauses, bool subsume,
               const PropagationConfig& propagation)
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
      propagated_row_eqn_(repr, num_vars),
      false_equation_(Equation::eq_false(repr, num_vars)),
      satisfied_probe_eqn_(repr, num_vars),
      propagate_miss_rate_(propagation.miss_rate),
      propagate_window_(propagation.window),
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
#ifdef NDEBUG
    tracked_used_row_idxs_ = propagate_miss_rate_ != 0.0 ? &used_row_idxs_ : nullptr;
#else
    // Debug checks the attribution whether or not a budget is on.
    tracked_used_row_idxs_ = &used_row_idxs_;
#endif
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
    assert(propagated_row_eqn_.num_bits() == active_num_vars_ + 1);

    selector_->set_attempt_budget(attempt_budget());
    force_complete_sweep_ = false;

    while (true) {
        const std::optional<ClauseSelection> selection = selector_->next_with_hint();
        if (!selection.has_value()) {
            counters_.num_deferred_runs += selector_->last_stop_was_budget() ? 1 : 0;
            return false;
        }
        assert(selection->id < selector_->num_clauses() && "selector returned an unknown clause");
        // A clause an earlier level already satisfies can only tell us so again.
        if (clause_is_known_satisfied(selector_->clause(selection->id))) {
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
    assert(selector_->is_clause_active(id) && "propagating from an inactive clause");
    Clause& clause = selector_->clause(id);
    ++counters_.num_attempt_props;

    Equation* propagated_eqn = propagator_.unit_propagate_one_clause(
        id, clause, trail_, trigger_hint, counters_.num_watch_updates, *selector_,
        tracked_used_row_idxs_);
    if (propagated_eqn == nullptr) {
        if (propagator_.last_attempt_found_satisfied()) {
            const std::size_t satisfied_level =
                shallowest_satisfying_level(clause.row(propagator_.last_satisfying_row_idx()));
            clause.satisfied_level = satisfied_level;
            clause.satisfied_stamp = level_stamp_[satisfied_level];
        }
        return PropagationOutcome::nothing;
    }

    assert(propagated_eqn->num_bits() == active_num_vars_ + 1);

    const std::size_t current_level = trail_.level();
    std::size_t level = current_level;
    if (tracked_used_row_idxs_ != nullptr) {
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
    if (conflicting && level < current_level) {
        // The levels above hold nothing the analysis may keep, so discarding
        // them here lets the `0 = 1` go in at the level it holds at.
        backjump_to(level);
    }
    // A clause only the top level falsifies was propagating before that level's
    // decision was taken, so the decision goes and the clause is met again on
    // the trail it was missed on.
    if (conflicting && level > 0 && budget_.has_value() && falsified_only_at_top_level(clause)) {
        ++counters_.num_undone_decisions;
        backjump_to(level - 1);
        decisions_->update_with_undone_decision();
        return propagate_from_clause(id, std::nullopt, true);
    }

    ++counters_.num_propagations;
    const bool below_current_level = level < current_level;
    counters_.num_out_of_order_learns += below_current_level ? 1 : 0;
    if (budget_.has_value()) {
        budget_->note_learn(below_current_level || count_learn_as_missed);
    }

    const Equation* learned_eqn = propagated_eqn;
    if (budget_.has_value()) {
        // The raw row, whose reduction `Trail::learn` redoes against the rows
        // at `level` and below.
        propagated_row_eqn_.assign_from(clause.row(propagator_.last_propagating_row_idx()));
        propagated_row_eqn_.plus_assign(false_equation_);
        learned_eqn = &propagated_row_eqn_;
    } else {
        // Re-normalize: the propagator reduced the clause row it propagated
        // from, not the negation it returns, so the result can still hold
        // span rows.
        (void)trail_.span().partial_reduce(*propagated_eqn);
    }
    const std::size_t trail_idx = trail_.learn(*learned_eqn, id, level);
    // The selector is notified even on the propagation that conflicts, and
    // with the row as stored, which is the equation reduced by its own level.
    selector_->update_with_propagation(
        trail_.span().row(trail_.trail_idx_to_row_idx()[trail_idx]), trail_);

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
    const std::size_t num_clauses = std::max<std::size_t>(1, selector_->num_clauses());
    budget_->set_num_clauses(num_clauses);
    counters_.final_budget_fraction = budget_->fraction_of_clauses();
    counters_.sum_budget_fraction += counters_.final_budget_fraction;
    ++counters_.num_budgeted_runs;
    const std::size_t attempts = budget_->attempts();
    // A budget the length of a whole sweep is no budget: only quiescence can end
    // such a run.
    return attempts >= num_clauses ? 0 : attempts;
}

std::optional<Solution> Solver::handle_conflict() {
    ++counters_.num_conflicts;
    assert(trail_.level() == conflict_level_ &&
           "the conflicting propagation left the trail above the level the conflict holds at");
    if (level() == 0) {
        write_root_unsat_proof();
        return Solution{SolveResult::unsat, std::nullopt};
    }
    handle_non_root_conflict();
    maybe_delete_clauses();
    maybe_report();
    return std::nullopt;
}

void Solver::handle_non_root_conflict() {
    AssertingClause learned = find_asserting_clause(
        trail_, selector_->clauses(), conflict_clause_id_, *decisions_, representative_rng_,
        minimize_, deletion_.enabled, bump_responsible_clauses_, subsume_);
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
    proof_writer_->write_empty(
        next_proof_clause_id_++,
        proof_ids(find_root_conflict_reasons(trail_, selector_->clauses(), conflict_clause_id_,
                                             representative_rng_)));
}

void Solver::write_root_unit(ClauseId reason_id, std::size_t trail_idx) {
    assert(proof_writer_ != nullptr && "no proof to record a root unit in");
    assert(trail_.level_of(trail_idx) == 0 && "the recorded row is not a root unit");

    // Reduced by the units before it, the row is what the compaction record
    // holds, so a pivot there names exactly this line.
    const RawRow trail_row =
        lift_row(trail_.span().row(trail_.trail_idx_to_row_idx()[trail_idx]),
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
    assert(proof_root_units_.size() == trail_.rows_at_level(0) &&
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

std::size_t Solver::shallowest_satisfying_level(const Equation& row) {
    satisfied_probe_eqn_.assign_from(row);
    const IndexSet used_row_idxs = trail_.span().reduce_get_coefficients(satisfied_probe_eqn_);
    assert(satisfied_probe_eqn_.is_eq_false() && "the row does not reduce to 0 = 1");
    return level_of_used_rows(used_row_idxs);
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
    decision_clause_of_level_.resize(level);
    did_complete_sweep_ = false;
}

Solver::DecisionStep Solver::make_decision_step() {
    DecisionResult decision =
        decisions_->make_decision(trail_.span(), selector_->clauses(), selector_->active_flags());
    if (decision.propagating_clause.has_value()) {
        propagating_clause_id_ = *decision.propagating_clause;
        return DecisionStep::propagating;
    }
    if (!decision.equation.has_value()) {
        return DecisionStep::exhausted;
    }
    Equation decision_eqn = *std::move(decision.equation);
    [[maybe_unused]] const ReduceResult reduced = trail_.span().partial_reduce(decision_eqn);
    assert(decision_eqn.num_bits() == active_num_vars_ + 1);
    // A decision must be neither 0 (already implied) nor 1 (already refuted).
    assert(reduced.kind == ReduceResult::Kind::leading &&
           "the decision procedure returned an equation the trail already decides");

    trail_.learn(decision_eqn, std::nullopt, trail_.level() + 1);
    level_stamp_.resize(trail_.level() + 1);
    level_stamp_[trail_.level()] = next_level_stamp_++;
    decision_clause_of_level_.push_back(decisions_->decision_clause_hint());
    assert(decision_clause_of_level_.size() == trail_.level() &&
           "the decision clause record fell out of step with the trail");
    selector_->update_with_decision(decision_eqn, trail_);
    selector_->update_with_decision_clause(decisions_->decision_clause_hint());
    return DecisionStep::decided;
}

#ifndef NDEBUG
bool Solver::every_active_clause_is_satisfied(const RowSpace& model) const {
    Equation reduced(repr_, active_num_vars_);
    for (ClauseId id = 0; id < selector_->num_clauses(); ++id) {
        if (!selector_->is_clause_active(id)) {
            continue;
        }
        bool satisfied = false;
        for (const Equation& row : selector_->clause(id)) {
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
        .remaining_vars = active_num_vars_ - trail_.rows_at_level(0),
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
    propagated_row_eqn_.resize(active_num_vars_);
    satisfied_probe_eqn_.resize(active_num_vars_);

    // The root trail is thrown away on purpose; see the warning on this method.
    trail_ = Trail(repr_, active_num_vars_);
    did_complete_sweep_ = false;
    level_stamp_.assign(1, 1);
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
    const std::size_t level = trail_.level_of(trail_idx);
    assert(level > 0 && trail_.rows_at_level(level - 1) == trail_idx &&
           "no decision level opens at this trail row");
    return decision_clause_of_level_[level - 1];
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

    if (propagate_miss_rate_ != 0.0) {
        budget_.emplace(propagate_miss_rate_, propagate_window_,
                        std::max<std::size_t>(1, selector_->num_clauses()));
    }

    solve_start_ = std::chrono::steady_clock::now();
    if (reports_progress()) {
        report_progress();
    }

    while (true) {
        if (propagate_until_conflict()) {
            if (const std::optional<Solution> solution = handle_conflict()) {
                return *solution;
            }
            continue;
        }

        // Quiescent at full rank: the trail is a total assignment.
        if (trail_.span().rows() == active_num_vars_) {
            if (did_complete_sweep_ || !selector_->last_stop_was_budget()) {
                return complete_satisfying_assignment();
            }
            // Full rank reached on a budgeted run is not yet quiescence, and one
            // unbudgeted run settles it. The flag is what keeps that from
            // repeating.
            did_complete_sweep_ = true;
            force_complete_sweep_ = true;
            continue;
        }

        if (compaction_.enabled && maybe_compact_before_decision()) {
            // The active numbering changed, so re-propagate rather than decide.
            continue;
        }

        switch (make_decision_step()) {
            case DecisionStep::decided:
                break;
            case DecisionStep::propagating: {
                ++counters_.num_decision_propagations;
                // Unit or falsified modulo the trail, so the watch logic has an
                // equation to return whichever pair of rows it is watching.
                const PropagationOutcome outcome =
                    propagate_from_clause(propagating_clause_id_);
                assert(outcome != PropagationOutcome::nothing &&
                       "a clause the decision step handed back propagated nothing");
                if (outcome == PropagationOutcome::conflicting) {
                    ++counters_.num_decision_conflicts;
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
    assert(every_active_clause_is_satisfied(complete) &&
           "the model reported as satisfying falsifies an active clause");

    IndexSet original_assignment = reconstruct_original_assignment(
        complete.unique_solution(), compaction_history_, active_var_to_original_,
        original_num_vars_);
    return Solution{SolveResult::sat, std::move(original_assignment)};
}

} // namespace xorcle
