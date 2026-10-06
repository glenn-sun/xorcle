#include "core/solver.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
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
               const PropagationConfig& propagation,
               const ImplicationGraphConfig& implication_graph)
    : repr_(repr),
      original_num_vars_(num_vars),
      active_num_vars_(num_vars),
      active_var_to_original_(num_vars),
      dropped_col_idxs_(num_vars + 1),
      compaction_(compaction),
      deletion_(deletion),
      trail_(repr, num_vars),
      selector_(std::move(selector)),
      decisions_(std::move(decisions)),
      restarts_(std::move(restarts)),
      propagator_(repr, num_vars),
      proof_writer_(proof_writer),
      proof_root_span_(repr, num_vars),
      propagate_miss_rate_(propagation.miss_rate),
      propagate_window_(propagation.window),
      propagated_row_eqn_(repr, num_vars),
      false_equation_(Equation::eq_false(repr, num_vars)),
      satisfied_probe_eqn_(repr, num_vars),
      progress_(progress),
      minimize_(minimize),
      bump_responsible_clauses_(bump_responsible_clauses),
      subsume_(subsume),
      implication_graph_config_(implication_graph),
      implication_graph_(repr, num_vars, implication_graph) {
    assert(selector_ != nullptr && "the clause selector must not be null");
    assert(decisions_ != nullptr && "the decision procedure must not be null");
    assert(restarts_ != nullptr && "the restart policy must not be null; use NoRestarts");
    cyclic_selector_ = dynamic_cast<Cyclic*>(selector_.get());
    assert((propagate_miss_rate_ == 0.0 || cyclic_selector_ != nullptr) &&
           "an attempt budget needs the cyclic selector");
    assert((proof_writer_ == nullptr || proof_writer_->num_vars() == original_num_vars_) &&
           "the proof writer's variable numbering must be the instance's");
    clause_activity_ = dynamic_cast<const ClauseActivitySource*>(decisions_.get());
    assert((!deletion_.enabled || deletion_.key != DeletionKey::activity ||
            clause_activity_ != nullptr) &&
           "the activity key needs a decision procedure that scores clauses");
#ifdef NDEBUG
    tracked_used_row_idxs_ = propagate_miss_rate_ != 0.0 ? &used_row_idxs_ : nullptr;
#else
    // Debug checks the attribution whether or not a budget is on.
    tracked_used_row_idxs_ = &used_row_idxs_;
#endif
    // The run's seed lives on the decision procedure, so these wait for its null check.
    representative_rng_ = make_rng(decisions_->base_seed(), RngComponent::minimize_representative);
    implication_graph_.set_base_seed(decisions_->base_seed());
    std::iota(active_var_to_original_.begin(), active_var_to_original_.end(), std::size_t{0});
    if (proof_writer_ != nullptr) {
        proof_col_idx_to_unit_id_.resize(num_vars);
    }
}

void Solver::add_starting_clause(Clause clause) {
    assert(!has_solved_ && "input clauses must be added before solving");
    assert(compaction_history_.empty() && "input clauses must be added before any compaction");
    assert(clause.cols() == active_num_vars_ + 1 &&
           "a starting clause must be over the solver's variable count");
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

    const ClauseId new_clause_id = selector_->num_clauses();
    decisions_->update_with_new_clause(clause, new_clause_id);
    selector_->add_starting_clause(std::move(clause));
    ++num_active_clauses_;
    if (proof_writer_ != nullptr) {
        proof_id_of_clause_.push_back(next_proof_clause_id_++);
    }
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
            counters_.num_deferred_runs += last_run_stopped_on_budget() ? 1U : 0U;
            assert((budget_.has_value() || debug_nothing_left_to_propagate()) &&
                   "propagation stopped short of a fixed point with no budget");
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

    const std::size_t current_level = trail_.level();
    // A propagation holds at the current level or below, so at the root there is
    // nothing an attribution could find.
    const bool attribute = tracked_used_row_idxs_ != nullptr && current_level != 0;

    Equation* propagated_eqn = propagator_.unit_propagate_one_clause(
        id, clause, trail_, trigger_hint, counters_.num_watch_updates, *selector_,
        attribute ? tracked_used_row_idxs_ : nullptr);
    if (propagated_eqn == nullptr) {
        if (propagator_.last_attempt_found_satisfied()) {
            const std::size_t satisfied_level =
                shallowest_satisfying_level(clause.row(propagator_.last_satisfying_row_idx()));
            clause.satisfied_level = satisfied_level;
            clause.satisfied_stamp = level_stamp_[satisfied_level];
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
        return propagate_from_clause(id, std::nullopt, true);
    }

    ++counters_.num_propagations;
    const bool below_current_level = level < current_level;
    counters_.num_out_of_order_learns += below_current_level ? 1 : 0;
    if (budget_.has_value()) {
        budget_->note_learn(below_current_level || count_learn_as_missed);
    }

    const Equation* learned_eqn = propagated_eqn;
    if (below_current_level) {
        // The propagator reduced against the levels above `level` too, so the
        // raw row goes in and `Trail::learn` reduces it against the rows at
        // `level` and below.
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
    selector_->update_with_propagation(trail_.row(trail_idx), trail_);

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
            learned.clause.rows() < learned.width_before_minimize ? 1U : 0U;
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
    // Sampled at the conflict, before the backjump.
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
                                                      decision_clause_of_level_,
                                                      satisfaction_record());
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
        proof_id_of_clause_.push_back(next_proof_clause_id_++);
        proof_writer_->write_learned(proof_id_of_clause_.back(), learned.clause,
                                     active_var_to_original_, proof_ids(learned.reason_ids));
    }

    const AssertingClauseInfo learned_info{.lbd = learned.lbd, .removed_levels = removed_levels};
    counters_.num_learned_width_1 += learned.clause.rows() == 1 ? 1U : 0U;
    counters_.num_learned_width_2 += learned.clause.rows() == 2 ? 1U : 0U;
    selector_->add_asserting_clause(std::move(learned.clause), trail_, learned_info);
    ++num_active_clauses_;

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
        assert(selector_->is_clause_active(record.clause_id) &&
               "a conflict reason must be active");
        if (reason_still_named(record)) {
            continue;
        }
        // A replacement takes the subsumed clause's standing, and so on along
        // chains of replacements: what replaces an input clause is no more a
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
            counters_.num_subsume_narrowed_to_width_2 +=
                record.replacement->rows() <= 2 ? 1U : 0U;
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
        selector_->set_clause_active(record.clause_id, false);
        selector_->replace_clause(record.clause_id,
                                  inactive_clause_placeholder(repr_, false_equation_));
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
        lift_row(trail_.row(trail_idx), active_var_to_original_, original_num_vars_);
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
                proof_col_idx_to_unit_id_[*proof_root_span_.row(row_idx).leading_bit()]);
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
    proof_col_idx_to_unit_id_[*proof_root_span_.row(*row_idx).leading_bit()] = unit_id;
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
    const auto start = std::chrono::steady_clock::now();
    // The last propagation run ended without a conflict, so how it stopped says
    // whether it reached quiescence.
    const SatisfactionRecord known(level_stamp_, trail_.level(), !last_run_stopped_on_budget());
    DecisionResult decision = decisions_->make_decision(trail_.span(), selector_->clauses(),
                                                        selector_->active_flags(), known);
    const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start;
    counters_.decision_time_s += elapsed.count();
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

    trail_.learn(decision_eqn, std::nullopt, trail_.level() + 1);
    level_stamp_.resize(trail_.level() + 1);
    level_stamp_[trail_.level()] = next_level_stamp_++;
    decision_clause_of_level_.push_back(decisions_->decision_clause_hint());
    assert(decision_clause_of_level_.size() == trail_.level() &&
           "the decision clause record fell out of step with the trail");
    selector_->update_with_decision(decision_eqn, trail_);
    if (cyclic_selector_ != nullptr) {
        cyclic_selector_->update_with_decision_clause(decision_clause_of_level_.back());
    }
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

bool Solver::debug_nothing_left_to_propagate() {
    const std::size_t words = active_num_vars_ / 64 + 1;
    const std::size_t stride = 1 + num_active_clauses_ * words * words / 2048;
    if (++debug_quiescences_ % stride != 0) {
        return true;
    }
    Equation first(repr_, active_num_vars_);
    Equation reduced(repr_, active_num_vars_);
    for (ClauseId id = 0; id < selector_->num_clauses(); ++id) {
        if (!selector_->is_clause_active(id)) {
            continue;
        }
        // Fully reduced rows are canonical, so two distinct nonzero ones span two
        // dimensions, and one alone propagates unless it is `0 = 1`.
        bool have_first = false;
        bool two_dimensions = false;
        for (const Equation& row : selector_->clause(id)) {
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
        .remaining_vars = active_num_vars() - trail_.rows_at_level(0),
        .decisions = decisions_->counters().decisions,
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
    CompactionState state{
        .trail = trail_,
        .clauses = selector_->clauses(),
        .candidate_clause_ids = {},
        .active_num_vars = active_num_vars_,
    };
    if (!should_compact_before_decision(state)) {
        return false;
    }
    const std::vector<ClauseId> candidate_clause_ids =
        selector_->clause_ids_carrying(trail_.span().pivot_col_idxs());
    state.candidate_clause_ids = candidate_clause_ids;

    const bool renumbers =
        renumbering_frees_enough_words(active_num_vars_, num_dropped_vars_ + trail_.span().rows());
    apply_substitution(build_substitution_plan(state));
    if (renumbers) {
        apply_renumbering();
    }
    return true;
}

void Solver::apply_substitution(SubstitutionPlan plan) {
    // The root trail is about to go, and the proof needs to know its units.
    if (proof_writer_ != nullptr) {
        write_compaction_units();
    }

    for (std::size_t i = 0; i < plan.rewritten_clause_ids.size(); ++i) {
        const ClauseId clause_id = plan.rewritten_clause_ids[i];
        assert(selector_->is_clause_active(clause_id) && "the plan rewrote an inactive clause");
        const std::size_t width_before = selector_->clause(clause_id).rows();
        if (plan.rewritten_is_active[i] == 0) {
            selector_->set_clause_active(clause_id, false);
            --num_active_clauses_;
        }
        selector_->replace_clause(clause_id, std::move(plan.rewritten_clauses[i]));
        if (plan.rewritten_is_active[i] != 0 && width_before > 2 &&
            selector_->clause(clause_id).rows() <= 2) {
            ++counters_.num_compaction_narrowed_to_width_2;
        }
    }
#ifndef NDEBUG
    // A clause the root trail satisfied carries one of its pivots, so no record
    // of that outlives the rewrite.
    for (ClauseId clause_id = 0; clause_id < selector_->num_clauses(); ++clause_id) {
        const Clause& clause = selector_->clause(clause_id);
        assert((!selector_->is_clause_active(clause_id) || clause.satisfied_stamp == 0 ||
                clause.satisfied_level != 0) &&
               "a clause the substitution left alone is recorded satisfied at the root");
    }
#endif

    // The root trail does not span `0 = 1`, so each of its rows drops a variable.
    dropped_col_idxs_.or_with(trail_.span().pivot_col_idxs());
    num_dropped_vars_ += trail_.span().rows();
    counters_.num_dropped_vars += trail_.span().rows();
    counters_.num_substituted_clauses += plan.rewritten_clause_ids.size();
    compaction_history_.push_back(std::move(plan.history_entry));

    // Its arcs are backed by clauses that may since have been rewritten.
    implication_graph_.reconfigure(active_num_vars_);

    // The root trail is thrown away on purpose; see the warning on this method.
    trail_ = Trail(repr_, active_num_vars_);
    did_complete_sweep_ = false;
    level_stamp_.assign(1, 1);
    decision_clause_of_level_.clear();

    selector_->update_with_substitution(plan.rewritten_clause_ids, dropped_col_idxs_, trail_);
    decisions_->update_with_substitution(dropped_col_idxs_, selector_->clauses(),
                                         selector_->active_flags());
    ++counters_.num_compactions;
}

void Solver::apply_renumbering() {
    assert(trail_.span().rows() == 0 && "a renumbering does not follow a substitution");
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
    implication_graph_.reconfigure(active_num_vars_);
    false_equation_ = Equation::eq_false(repr_, active_num_vars_);
    propagated_row_eqn_.resize(active_num_vars_);
    satisfied_probe_eqn_.resize(active_num_vars_);
    trail_ = Trail(repr_, active_num_vars_);

    selector_->update_with_renumbering(transform, trail_);
    decisions_->update_with_renumbering(transform);
    compaction_history_.back().renumbering = transform;
    ++counters_.num_renumberings;
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
                                                     .activity = clause_activity_}));
}

void Solver::apply_deletion(const DeletionPlan& plan) {
    assert(plan.unused_passes.size() == selector_->num_clauses() &&
           "the deletion plan does not cover every clause");

    for (const ClauseId clause_id : selector_->active_clause_ids()) {
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
        selector_->set_clause_active(clause_id, false);
        selector_->replace_clause(clause_id, inactive_clause_placeholder(repr_, false_equation_));
    }

    ++counters_.num_deletion_passes;
    num_active_clauses_ -= plan.deleted.size();
    counters_.num_deleted_clauses += plan.deleted.size();
    counters_.num_demoted_clauses += plan.demoted.size();
}

bool Solver::maybe_run_implication_graph() {
    if (!implication_graph_config_.enabled) {
        return false;
    }
    if (!implication_graph_run_is_due_) {
        const bool by_clauses = clauses_reaching_width_at_most_2() -
                                    clauses_at_last_implication_graph_run_ >=
                                implication_graph_config_.clause_cadence;
        const bool by_conflicts = counters_.num_conflicts -
                                      conflicts_at_last_implication_graph_run_ >=
                                  implication_graph_config_.cadence;
        if (follow_up_is_pending_) {
            // The first call after the feeding run: its units have propagated and
            // been compacted away, and nothing has been decided since.
            assert(trail_.level() == 0 && trail_.span().rows() == 0 && !by_conflicts &&
                   "the follow-up test ran after a decision or before the compaction");
            follow_up_is_pending_ = false;
            if (by_clauses) {
                implication_graph_run_is_due_ = true;
                run_is_due_from_ = RunTrigger::feed;
                conflicts_at_run_became_due_ = counters_.num_conflicts;
            } else {
                ++implication_graph_counters_.num_follow_ups_suppressed;
            }
        } else if (by_clauses || by_conflicts) {
            implication_graph_run_is_due_ = true;
            run_is_due_from_ = by_clauses ? RunTrigger::clauses : RunTrigger::conflicts;
            // The conflict interval elapsed at a conflict count this knows; the
            // clause count is only read here, so that is the earliest it can be
            // credited with.
            conflicts_at_run_became_due_ =
                by_conflicts
                    ? conflicts_at_last_implication_graph_run_ + implication_graph_config_.cadence
                    : counters_.num_conflicts;
        }
    }
    // A run adds its units to the root trail, so like a compaction it waits for
    // the trail to be back at the root.
    if (!implication_graph_run_is_due_ || trail_.level() != 0) {
        return false;
    }
    implication_graph_run_is_due_ = false;
    const std::size_t conflicts_waited = counters_.num_conflicts - conflicts_at_run_became_due_;

    const auto start = std::chrono::steady_clock::now();
    // Every trail row is at the root level, so the trail's span is the root span.
    const ImplicationGraphRun summary = implication_graph_.run(
        selector_->clauses(), selector_->active_clause_ids(), trail_.span(),
        proof_writer_ != nullptr ? &implication_graph_proof_ : nullptr);
    const std::vector<Equation>& units = summary.units;
    const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start;

    implication_graph_counters_.time_s += elapsed.count();
    implication_graph_counters_.span_intersection_time_s += summary.span_intersection_time_s;
    implication_graph_counters_.num_runs += summary.did_work ? 1 : 0;
    implication_graph_counters_.num_edges_visited += summary.edges_visited;
    implication_graph_counters_.num_pairs_eligible += summary.pairs_eligible;
    implication_graph_counters_.num_pairs_examined += summary.pairs_examined;
    implication_graph_counters_.num_refuted_sides += summary.refuted_sides;
    implication_graph_counters_.num_runs_at_the_pair_cap += summary.at_the_pair_cap ? 1 : 0;
    implication_graph_counters_.num_truncated_sides += summary.truncated_sides;
    if (summary.did_work) {
        implication_graph_counters_.num_runs_after_a_conflict +=
            counters_.num_conflicts != 0 ? 1 : 0;
        implication_graph_counters_.num_conflicts_waited += conflicts_waited;
        implication_graph_counters_.max_conflicts_waited =
            std::max(implication_graph_counters_.max_conflicts_waited, conflicts_waited);
        switch (run_is_due_from_) {
            case RunTrigger::clauses:
                ++implication_graph_counters_.num_runs_due_to_clauses;
                break;
            case RunTrigger::conflicts:
                ++implication_graph_counters_.num_runs_due_to_conflicts;
                break;
            case RunTrigger::feed:
                ++implication_graph_counters_.num_runs_due_to_a_feed;
                break;
            case RunTrigger::first_run:
                break;
        }
    }

    if (units.empty()) {
        conflicts_at_last_implication_graph_run_ = counters_.num_conflicts;
        clauses_at_last_implication_graph_run_ = clauses_reaching_width_at_most_2();
        return false;
    }

    std::vector<ClauseId> unit_lines;
    if (proof_writer_ != nullptr) {
        unit_lines = write_implication_graph_lines(implication_graph_proof_);
        assert(unit_lines.size() == units.size() && "a fed unit went without a proof line");
    }
    for (std::size_t i = 0; i < units.size(); ++i) {
        feed_unit(units[i], proof_writer_ != nullptr ? unit_lines[i] : 0);
    }
    implication_graph_counters_.num_units_fed += units.size();
    conflicts_at_last_implication_graph_run_ = counters_.num_conflicts;
    clauses_at_last_implication_graph_run_ = clauses_reaching_width_at_most_2();
    follow_up_is_pending_ = true;
    return true;
}

void Solver::feed_unit(const Equation& unit, const ClauseId proof_line) {
    assert(trail_.level() == 0 && "a unit is fed at the root");
    assert(unit.num_bits() == active_num_vars_ + 1 &&
           "a fed unit is not over the active variable numbering");

    Clause clause(repr_, active_num_vars_);
    Equation row = unit;
    row.plus_assign(false_equation_);
    clause.add_row(std::move(row));
    assert(clause.rows() == 1 && "a fed unit did not make a width-1 clause");

    const ClauseId new_clause_id = selector_->num_clauses();
    if (proof_writer_ != nullptr) {
        assert(proof_id_of_clause_.size() == new_clause_id && "a clause went without a proof id");
        proof_id_of_clause_.push_back(proof_line);
    }
    decisions_->update_with_new_clause(clause, new_clause_id);
    selector_->add_asserting_clause(std::move(clause), trail_,
                                    AssertingClauseInfo{.lbd = 0, .removed_levels = 0});
    ++num_active_clauses_;
}

Solution Solver::solve() {
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

        if (compaction_.enabled && maybe_compact_before_decision()) {
            // The root trail was emptied, so re-propagate rather than decide.
            continue;
        }

        if (maybe_run_implication_graph()) {
            // The root trail gained units, so re-propagate rather than decide.
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
        complete.unique_solution(), compaction_history_, active_var_to_original_,
        original_num_vars_);
    return Solution{SolveResult::sat, std::move(original_assignment)};
}

void Solver::add_to_stats(RunStats& stats) const {
    stats.num_vars = num_vars();
    stats.active_vars = active_num_vars();

    stats.num_conflicts = counters_.num_conflicts;
    stats.num_propagations = counters_.num_propagations;
    stats.num_attempt_props = counters_.num_attempt_props;
    stats.num_watch_updates = counters_.num_watch_updates;
    stats.num_compactions = counters_.num_compactions;
    stats.num_renumberings = counters_.num_renumberings;
    stats.num_dropped_vars = counters_.num_dropped_vars;
    stats.num_substituted_clauses = counters_.num_substituted_clauses;
    stats.num_deferred_runs = counters_.num_deferred_runs;
    stats.num_out_of_order_learns = counters_.num_out_of_order_learns;
    stats.num_undone_decisions = counters_.num_undone_decisions;
    stats.num_decision_propagations = counters_.num_decision_propagations;
    stats.num_decision_conflicts = counters_.num_decision_conflicts;
    stats.mean_budget_fraction =
        counters_.num_budgeted_runs == 0
            ? 0.0
            : counters_.sum_budget_fraction / static_cast<double>(counters_.num_budgeted_runs);
    stats.final_budget_fraction = counters_.final_budget_fraction;

    stats.num_restarts = counters_.num_restarts;
    stats.num_reused_levels = counters_.num_reused_levels;

    // Ids are never reused, so what was allocated and what is still there part
    // company as soon as deletion runs.
    stats.clauses_allocated = num_clauses();
    stats.clauses_in_database = num_active_clauses();
    stats.average_learned_lbd =
        counters_.num_asserting_clauses == 0
            ? 0.0
            : static_cast<double>(counters_.sum_learned_lbd) /
                  static_cast<double>(counters_.num_asserting_clauses);

    stats.learned_equation_bits = counters_.learned_equation_bits;
    stats.learned_width_1 = counters_.num_learned_width_1;
    stats.learned_width_2 = counters_.num_learned_width_2;
    stats.subsume_narrowed_to_width_2 = counters_.num_subsume_narrowed_to_width_2;
    stats.compaction_narrowed_to_width_2 = counters_.num_compaction_narrowed_to_width_2;

    stats.num_minimized_clauses = counters_.num_minimized_clauses;
    stats.minimize_width_before = counters_.minimize_width_before;
    stats.minimize_width_after = counters_.minimize_width_after;

    stats.num_subsumed_clauses = counters_.num_subsumed_clauses;
    stats.num_subsumed_conflicting_clauses = counters_.num_subsumed_conflicting_clauses;
    stats.num_subsume_replacements = counters_.num_subsume_replacements;

    stats.num_deletion_passes = counters_.num_deletion_passes;
    stats.num_deleted_clauses = counters_.num_deleted_clauses;
    stats.num_demoted_clauses = counters_.num_demoted_clauses;
    stats.num_promoted_clauses = counters_.num_promoted_clauses;

    const ImplicationGraphCounters& graph = implication_graph_counters_;
    stats.implication_graph_runs = graph.num_runs;
    stats.implication_graph_edges_visited = graph.num_edges_visited;
    stats.implication_graph_pairs_eligible = graph.num_pairs_eligible;
    stats.implication_graph_pairs_examined = graph.num_pairs_examined;
    stats.implication_graph_refuted_sides = graph.num_refuted_sides;
    stats.implication_graph_runs_at_the_pair_cap = graph.num_runs_at_the_pair_cap;
    stats.implication_graph_truncated_sides = graph.num_truncated_sides;
    stats.implication_graph_units_fed = graph.num_units_fed;
    stats.implication_graph_runs_after_a_conflict = graph.num_runs_after_a_conflict;
    stats.implication_graph_runs_due_to_clauses = graph.num_runs_due_to_clauses;
    stats.implication_graph_runs_due_to_conflicts = graph.num_runs_due_to_conflicts;
    stats.implication_graph_runs_due_to_a_feed = graph.num_runs_due_to_a_feed;
    stats.implication_graph_follow_ups_suppressed = graph.num_follow_ups_suppressed;
    stats.implication_graph_conflicts_waited = graph.num_conflicts_waited;
    stats.implication_graph_max_conflicts_waited = graph.max_conflicts_waited;
    stats.implication_graph_width_2_clauses = implication_graph_.num_width_2_clauses();
    stats.implication_graph_vertex_pairs = implication_graph_.num_vertex_pairs();
    stats.implication_graph_span_intersection_time_s = graph.span_intersection_time_s;
    stats.implication_graph_time_s = graph.time_s;

    stats.decision_time_s = counters_.decision_time_s;

    selector_->add_to_stats(stats);
    decisions_->add_to_stats(stats);
    restarts_->add_to_stats(stats);
}

} // namespace xorcle
