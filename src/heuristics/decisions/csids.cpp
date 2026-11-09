#include "heuristics/decisions/csids.h"

#include <cassert>
#include <chrono>
#include <optional>
#include <utility>

namespace xorcle {

void CsidsActivity::bump_activity(ClauseId id) {
    activity_heap.increase_key(id, inc_amt);
    if (activity_heap.activity(id) > 1e100) {
        activity_heap.rescale(1e-100);
        inc_amt *= 1e-100;
    }
}

CsidsState::CsidsState(const CsidsConfig& config)
    : activity{.activity_heap = {},
               .inc_amt = config.bump,
               .initial_activity = config.initial_activity,
               .decay = config.decay},
      initial_inc_amt(config.bump) {
    assert(config.decay >= 0.0 && config.decay <= 1.0 && "CSIDS requires 0 <= decay <= 1");
    assert(config.focused_decay >= 0.0 && config.focused_decay <= 1.0 &&
           "CSIDS requires 0 <= focused decay <= 1");
    assert(config.initial_activity >= 0.0 && "CSIDS requires a non-negative A0");
    assert(config.bump >= 0.0 && "CSIDS requires a non-negative bump");
    assert(config.intersection_depth >= 1 && "CSIDS requires an intersection depth of at least 1");
}

void CsidsState::reset_activity() {
    activity.activity_heap.reset();
    for (ClauseId id = 0; id < num_clauses; ++id) {
        activity.activity_heap.set_activity(id, 0.0);
        activity.activity_heap.insert(id);
    }
    activity.inc_amt = initial_inc_amt;
    last_decision_clause.reset();
    pending_learned_priority.reset();
}

Csids::Csids(EquationRepr repr, std::size_t num_vars, Seed seed, const CsidsConfig& config)
    : DecisionProcedure(repr, num_vars, seed),
      state_(config),
      stable_decay_(config.decay),
      focused_decay_(config.focused_decay),
      clause_sat_workspace_(repr, num_vars),
      intersection_depth_(config.intersection_depth),
      span_(config.span),
      intersection_(repr, num_vars),
      clause_equation_rng_(make_rng(seed, RngComponent::csids_equation)),
      saved_trails_(repr, num_vars, config.phase_saving) {
    reinitialize();
}

void Csids::reinitialize() {
    reset_phase_rng();
    clause_equation_rng_ = make_rng(base_seed(), RngComponent::csids_equation);
    reset_counters();
    intersection_counters_ = {};
    state_.reset_activity();
    satisfied_by_level_.clear();
    level_ = 0;
    saved_trails_.reinitialize(num_vars());
}

DecisionResult Csids::make_decision(const RowSpace& trail_span,
                                    const std::vector<Clause>& clauses,
                                    const std::vector<std::uint8_t>& clause_active,
                                    const SatisfactionRecord& known) {
    assert(trail_span.cols() == num_vars() + 1 &&
           "the trail span does not match the procedure's variable count");
    assert(trail_span.repr() == repr() &&
           "the trail span does not match the procedure's representation");
    assert(trail_span.rows() < num_vars() && "no legal decision remains");
    assert(clause_active.size() == clauses.size() && "clause active flags are desynchronized");
    assert((!known.level().has_value() || *known.level() == level_) &&
           "CSIDS lost track of the trail's level");

    while (!state_.activity.activity_heap.empty()) {
        const ClauseId id = state_.activity.activity_heap.peek_max();
        if (id >= clauses.size() || clause_active[id] == 0) {
            // Stale: naming a clause since deleted or made inactive. Neither is
            // ever undone, so the id is no candidate again and the heap need not
            // carry it.
            state_.activity.activity_heap.extract_max();
            continue;
        }
        const Clause& clause = clauses[id];
        const ClauseStatus status = candidate_status(id, clause, trail_span, known);
        if (status == ClauseStatus::satisfied) {
            continue;
        }
        if (status == ClauseStatus::propagating) {
            // Still a candidate, so it stays in the heap, and it names no
            // decision, so the last-decision record stands.
            return DecisionResult{std::nullopt, id};
        }
        if (intersection_depth_ > 1 || consults_trails()) {
            const auto start = std::chrono::steady_clock::now();
            std::optional<DecisionResult> result =
                decide_from_intersection(id, trail_span, clauses, clause_active, known);
            const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start;
            intersection_counters_.time_s += elapsed.count();
            if (result.has_value()) {
                if (result->equation.has_value()) {
                    state_.last_decision_clause = id;
                    ++level_;
                }
                return std::move(*result);
            }
        }
        state_.last_decision_clause = id;

        SampledDecision sample =
            sample_equation_from_clause(clause, trail_span, clause_equation_rng_);

        record_decision();
        apply_decision_phase(sample.reduced);
        if (decision_phase() == DecisionPhase::rotate) {
            saved_trails_.note_falsify_decision();
        }
        ++level_;
        return DecisionResult{std::move(sample.reduced), std::nullopt};
    }

    state_.last_decision_clause.reset();
    return DecisionResult{};
}

std::optional<DecisionResult> Csids::decide_from_intersection(
    const ClauseId top_id, const RowSpace& trail_span, const std::vector<Clause>& clauses,
    const std::vector<std::uint8_t>& clause_active, const SatisfactionRecord& known) {
    MaxHeap& heap = state_.activity.activity_heap;
    assert(heap.peek_max() == top_id && "the top clause is not the heap's maximum");

    // Every reduced row is zero in the trail span's pivot columns, so a nonzero
    // combination of them lies outside the span; a clause is open only if `0 = 1`
    // is outside its span plus the trail's, so its negation lies outside too.
    // Under span^± the space also holds `0 = 1`, and a nonzero combination of
    // the other rows is a direction the trail does not settle.
    RowSpace& space = intersection_;
    std::vector<Equation>& spare_rows = intersection_spare_rows_;
    assert(space.rows() == 0 && "the last decision left rows in the intersection");
    // Rows in row-echelon order are stored as they are.
    for (const Equation& row : clause_sat_workspace_.reduced_clause()) {
        if (spare_rows.empty()) {
            space.add_row(row);
        } else {
            Equation copy = std::move(spare_rows.back());
            spare_rows.pop_back();
            copy.assign_from(row);
            space.add_row(std::move(copy));
        }
    }
    const std::size_t dimension_before = space.rows();
    const bool plus_minus = span_ == CsidsSpan::plus_minus;
    if (plus_minus) {
        add_false_row_to_intersection();
    }
    // `0 = 1` is not a direction.
    const auto dimension = [&space, plus_minus] {
        return space.rows() - (plus_minus ? 1 : 0);
    };

    // Open clauses are set aside to surface the ones below, and all go back.
    set_aside_ids_.clear();
    set_aside_ids_.push_back(top_id);
    changed_clause_ids_.clear();
    changed_clause_ids_.push_back(top_id);
    heap.extract_max();
    std::optional<ClauseId> propagating_id;
    std::size_t intersected_clauses = 0;
    while (set_aside_ids_.size() < intersection_depth_ && !heap.empty()) {
        // One dimension left is the decision, and after quiescence no clause
        // below is propagating either, so the rest of the walk finds nothing.
        if (dimension() == 1 && known.quiescent()) {
            break;
        }
        const ClauseId id = heap.peek_max();
        if (id >= clauses.size() || clause_active[id] == 0) {
            heap.extract_max();
            continue;
        }
        const ClauseStatus status = candidate_status(id, clauses[id], trail_span, known);
        if (status == ClauseStatus::satisfied) {
            ++intersection_counters_.passed_satisfied_clauses;
            continue;
        }
        if (status == ClauseStatus::propagating) {
            propagating_id = id;
            break;
        }
        set_aside_ids_.push_back(id);
        heap.extract_max();
        if (dimension() == 1) {
            // Its intersection with any clause is itself or nothing.
            continue;
        }

        const RowSpace& reduced_clause = clause_sat_workspace_.reduced_clause();
        // An equation of both spans leads in a pivot column of each. Under span^±
        // the clause's space is the reduced one plus `0 = 1`, which `space` holds,
        // so its intersection with `space` is `space` meeting the reduced clause,
        // plus `0 = 1`; and an open clause never pivots the constant column, so
        // `space`'s pivot there is never found shared.
        assert(!reduced_clause.pivot_col_idxs().test(reduced_clause.cols() - 1) &&
               "an open clause holds 0 = 1 modulo the trail");
        if (!space.pivot_col_idxs().intersects(reduced_clause.pivot_col_idxs())) {
            assert(span_intersection(space, reduced_clause).empty() &&
                   "spans sharing no pivot column intersect");
            ++intersection_counters_.empty_intersections;
            ++intersection_counters_.skipped_intersections;
            continue;
        }
        std::vector<Equation> intersection =
            span_intersection(space, reduced_clause, spare_rows);
        if (intersection.empty()) {
            ++intersection_counters_.empty_intersections;
            continue;
        }
        if (intersection.size() < dimension()) {
            changed_clause_ids_.push_back(id);
        }
        clear_intersection();
        for (Equation& row : intersection) {
            space.add_row(std::move(row));
        }
        if (plus_minus) {
            add_false_row_to_intersection();
        }
        ++intersected_clauses;
    }
    for (const ClauseId id : set_aside_ids_) {
        heap.insert(id);
    }

    if (propagating_id.has_value()) {
        ++intersection_counters_.propagating_clauses;
        clear_intersection();
        return DecisionResult{std::nullopt, propagating_id};
    }
    if (intersected_clauses == 0 && !consults_trails()) {
        clear_intersection();
        return std::nullopt;
    }
    if (intersected_clauses > 0) {
        ++intersection_counters_.decisions;
        intersection_counters_.intersected_clauses += intersected_clauses;
        intersection_counters_.dimension_before += dimension_before;
        intersection_counters_.dimension_after += dimension();
        if (dimension() == 1) {
            ++intersection_counters_.dimension_one;
        }
    }
    const bool consulted = consults_trails();
    if (consulted) {
        if (plus_minus) {
            // Phase saving takes V without `0 = 1`, and adds it back while the side is open.
            intersection_spare_rows_.push_back(space.pop_row(space.rows() - 1));
        }
        saved_trails_.narrow(space, changed_clause_ids_, clauses, trail_span,
                             known.level_stamps());
    }

    // `0 = 1`, while the space holds it, is the last row, and the side is drawn
    // after; a space phase saving left without it has its sides chosen.
    const bool holds_false = space.pivot_col_idxs().test(space.cols() - 1);
    assert((!holds_false || space.pivot_col_idx(space.rows() - 1) == space.cols() - 1) &&
           "0 = 1 is not the intersection's last row");
    Equation decision = Equation::eq_true(repr(), num_vars());
    random_nonzero_coefficients(space.rows() - (holds_false ? 1 : 0), clause_equation_rng_)
        .for_each_set([&](std::size_t row_idx) { decision.plus_assign(space.row(row_idx)); });
#ifndef NDEBUG
    Equation reduced = decision;
    assert(trail_span.partial_reduce(reduced).is_leading() &&
           "a decision from an intersection is not legal");
#endif
    clear_intersection();
    record_decision();
    if (holds_false || (span_ == CsidsSpan::plain && !consulted)) {
        const DecisionPhase phase = decision_phase();
        const bool rotate = phase == DecisionPhase::rotate;
        const bool sided = phase == DecisionPhase::falsify || phase == DecisionPhase::satisfy || rotate;
        if ((plus_minus && sided) || (rotate && holds_false)) {
            orient_to_falsify(decision, clauses[top_id], trail_span);
        }
        if (rotate && !consulted) {
            saved_trails_.note_falsify_decision();
        }
#ifndef NDEBUG
        if (sided) {
            RowSpace top_span = trail_span;
            for (const Equation& row : clauses[top_id]) {
                top_span.add_row(row);
            }
            Equation side = decision;
            assert(top_span.partial_reduce(side).kind == ReduceResult::Kind::eq_true &&
                   "the falsify side of a decision is outside its top clause's span(neg(C), U)");
        }
#endif
        apply_decision_phase(decision);
    }
    return DecisionResult{std::move(decision), std::nullopt};
}

void Csids::orient_to_falsify(Equation& eqn, const Clause& top_clause,
                              const RowSpace& trail_span) {
    [[maybe_unused]] const ClauseStatus status =
        clause_status_by_trail(trail_span, top_clause, clause_sat_workspace_);
    assert(status == ClauseStatus::open && "the top clause is no longer open at its decision");
    Equation side;
    if (intersection_spare_rows_.empty()) {
        side = eqn;
    } else {
        side = std::move(intersection_spare_rows_.back());
        intersection_spare_rows_.pop_back();
        side.assign_from(eqn);
    }
    const ReduceResult kind = clause_sat_workspace_.reduced_clause().partial_reduce(side);
    assert(!kind.is_leading() && "a decision under span^± is outside its top clause's span^±");
    if (kind.kind == ReduceResult::Kind::eq_false) {
        apply_decision_phase(eqn, DecisionPhase::satisfy);
    }
    intersection_spare_rows_.push_back(std::move(side));
}

void Csids::clear_intersection() {
    while (intersection_.rows() != 0) {
        intersection_spare_rows_.push_back(intersection_.pop_row(intersection_.rows() - 1));
    }
}

void Csids::add_false_row_to_intersection() {
    Equation row;
    if (intersection_spare_rows_.empty()) {
        row = Equation::eq_false(repr(), num_vars());
    } else {
        row = std::move(intersection_spare_rows_.back());
        intersection_spare_rows_.pop_back();
        row.set_eq_false();
    }
    [[maybe_unused]] const std::optional<std::size_t> row_idx =
        intersection_.add_row_get_row_idx(std::move(row));
    assert(row_idx == std::optional<std::size_t>(intersection_.rows() - 1) &&
           "0 = 1 is not the intersection's last row");
}

void Csids::update_with_asserting_clause(const Clause&) {
    state_.pending_learned_priority = state_.activity.initial_activity * state_.activity.inc_amt;
    if (state_.activity.decay == 0.0) {
        // Growing the increment is the lazy form of decaying every activity. At
        // zero the increment would have to be infinite, so decay them directly.
        state_.activity.activity_heap.rescale(0.0);
    } else {
        state_.activity.inc_amt *= (1.0 / state_.activity.decay);
        // A bump rescales first unless conflicts go unbumped, which a decay of 0.5
        // takes 170 of to reach this.
        if (state_.activity.inc_amt > 1e150) {
            state_.activity.activity_heap.rescale(1e-100);
            state_.activity.inc_amt *= 1e-100;
        }
    }
}

void Csids::update_with_mode(SearchMode mode) {
    state_.activity.decay = mode == SearchMode::stable ? stable_decay_ : focused_decay_;
    saved_trails_.update_with_mode(mode);
}

ClauseStatus Csids::candidate_status(const ClauseId id, const Clause& clause,
                                     const RowSpace& trail_span, const SatisfactionRecord& known) {
    std::optional<std::size_t> level = known.satisfying_level(clause);
    if (!level.has_value()) {
        const ClauseStatus status = clause_status_by_trail(trail_span, clause, clause_sat_workspace_);
        if (status != ClauseStatus::satisfied) {
            return status;
        }
        level = level_;
    }
    state_.activity.activity_heap.extract_max();
    if (satisfied_by_level_.size() <= *level) {
        satisfied_by_level_.resize(*level + 1);
    }
    satisfied_by_level_[*level].push_back(id);
    return ClauseStatus::satisfied;
}

void Csids::update_with_backjump(const std::size_t level) {
    assert(level <= level_ && "the backjump target is above the trail's level");
    for (std::size_t discarded = level + 1; discarded < satisfied_by_level_.size(); ++discarded) {
        for (const ClauseId id : satisfied_by_level_[discarded]) {
            state_.activity.activity_heap.insert(id);
        }
        satisfied_by_level_[discarded].clear();
    }
    level_ = level;
}

void Csids::update_with_new_clause(const Clause&, ClauseId id) {
    assert(id <= state_.num_clauses && "clause ids must be handed out consecutively");
    if (id < state_.num_clauses) {
        return;
    }
    const std::optional<double> learned_priority = state_.pending_learned_priority;
    for (ClauseId next_id = state_.num_clauses; next_id <= id; ++next_id) {
        double priority = 0.0;
        if (learned_priority.has_value() && next_id == id) {
            priority = *learned_priority;
        }
        state_.activity.activity_heap.set_activity(next_id, priority);
        state_.activity.activity_heap.insert(next_id);
    }
    state_.num_clauses = id + 1;
    state_.pending_learned_priority.reset();
}

void Csids::update_with_conflict(const Trail& trail, std::span<const std::size_t> level_stamps) {
    if (phase_saving()) {
        saved_trails_.update_with_conflict(trail, level_stamps);
    }
}

void Csids::update_with_resolved_clause(ClauseId id) {
    if (id < state_.num_clauses) {
        state_.activity.bump_activity(id);
    }
}

void Csids::update_with_subsumed_clause(ClauseId subsumed_id, ClauseId replacement_id) {
    assert(subsumed_id < state_.num_clauses && replacement_id < state_.num_clauses &&
           "subsumption names an unregistered clause");
    const double inherited = clause_activity(subsumed_id);
    if (inherited > clause_activity(replacement_id)) {
        state_.activity.activity_heap.set_activity(replacement_id, inherited);
    }
}

void Csids::update_with_narrowed_clause(ClauseId id) {
    assert(id < state_.num_clauses && "a narrowing names an unregistered clause");
    state_.activity.activity_heap.insert(id);
}

void Csids::add_to_stats(RunStats& stats) const {
    DecisionProcedure::add_to_stats(stats);
    stats.intersection_decisions = intersection_counters_.decisions;
    stats.intersected_clauses = intersection_counters_.intersected_clauses;
    stats.intersection_dimension_before = intersection_counters_.dimension_before;
    stats.intersection_dimension_after = intersection_counters_.dimension_after;
    stats.intersection_dimension_one = intersection_counters_.dimension_one;
    const PhaseSavingCounters& saved = saved_trails_.counters();
    stats.phase_saving_decisions = saved.decisions;
    stats.phase_saving_clause_step_target = saved.target_clause_step.accepted;
    stats.phase_saving_clause_step_target_trivial = saved.target_clause_step.trivial;
    stats.phase_saving_clause_step_target_inconsistent = saved.target_clause_step.inconsistent;
    stats.phase_saving_clause_step_recent = saved.recent_clause_step.accepted;
    stats.phase_saving_clause_step_recent_trivial = saved.recent_clause_step.trivial;
    stats.phase_saving_clause_step_recent_inconsistent = saved.recent_clause_step.inconsistent;
    stats.phase_saving_clause_step_top_clause = saved.clause_step_top_clause;
    stats.phase_saving_clause_step_partner = saved.clause_step_partner;
    stats.phase_saving_narrowing_target = saved.target_narrowing;
    stats.phase_saving_narrowing_recent = saved.recent_narrowing;
    stats.phase_saving_none = saved.none;
    stats.phase_saving_uniform_sides = saved.uniform_sides;
    stats.phase_saving_falsify_sides = saved.falsify_sides;
    stats.phase_saving_rotations = saved.rotations;
    stats.phase_saving_decisions_target_recent_falsify =
        saved.order_decisions[static_cast<std::size_t>(PhaseOrder::target_recent_falsify)];
    stats.phase_saving_decisions_falsify =
        saved.order_decisions[static_cast<std::size_t>(PhaseOrder::falsify)];
    stats.phase_saving_decisions_target_falsify =
        saved.order_decisions[static_cast<std::size_t>(PhaseOrder::target_falsify)];
    stats.phase_saving_decisions_recent_falsify =
        saved.order_decisions[static_cast<std::size_t>(PhaseOrder::recent_falsify)];
    stats.phase_saving_dimension_before = saved.dimension_before;
    stats.phase_saving_dimension_after = saved.dimension_after;
    stats.phase_saving_cleared = saved.cleared;
    stats.phase_saving_time_s = saved.time_s;
    stats.intersection_empty = intersection_counters_.empty_intersections;
    stats.intersection_skipped = intersection_counters_.skipped_intersections;
    stats.intersection_passed_satisfied_clauses = intersection_counters_.passed_satisfied_clauses;
    stats.intersection_propagating_clauses = intersection_counters_.propagating_clauses;
    stats.intersection_time_s = intersection_counters_.time_s;
}

double Csids::clause_activity(ClauseId id) const {
    return state_.activity.activity_heap.activity(id);
}

std::optional<ClauseId> Csids::decision_clause_hint() const {
    return state_.last_decision_clause;
}

void Csids::update_with_substitution(const IndexSet&, const std::vector<Clause>& clauses,
                                     const std::vector<std::uint8_t>& clause_active) {
    state_.num_clauses = static_cast<ClauseId>(clauses.size());
    if (phase_saving()) {
        saved_trails_.clear();
    }

    // The root trail is rebuilt, so no clause is known satisfied any more.
    for (std::vector<ClauseId>& ids : satisfied_by_level_) {
        for (const ClauseId id : ids) {
            state_.activity.activity_heap.insert(id);
        }
        ids.clear();
    }
    level_ = 0;

    if (state_.last_decision_clause.has_value()) {
        const ClauseId id = *state_.last_decision_clause;
        if (id >= clause_active.size() || clause_active[id] == 0) {
            state_.last_decision_clause.reset();
        }
    }
}

void Csids::update_with_renumbering(const CompactionTransform& transform) {
    assert(transform.old_num_vars() == num_vars() &&
           "the transform was built for a different active variable numbering");
    set_num_vars(transform.new_num_vars());
    clause_sat_workspace_.reconfigure(num_vars());
    intersection_ = RowSpace(repr(), num_vars());
    intersection_spare_rows_.clear();
    saved_trails_.reconfigure(num_vars());
}

} // namespace xorcle
