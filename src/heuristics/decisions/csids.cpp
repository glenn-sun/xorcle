#include "heuristics/decisions/csids.h"

#include <cassert>
#include <optional>
#include <utility>

namespace xorcle {

void Csids::bump_activity(ClauseId id) {
    heap_.increase_key(id, config_.bump * scale_);
    if (heap_.activity(id) > 1e100) {
        heap_.rescale(1e-100);
        scale_ *= 1e-100;
    }
}

Csids::Csids(EquationRepr repr, std::size_t num_vars, Seed seed, const CsidsConfig& config)
    : repr_(repr),
      num_vars_(num_vars),
      base_seed_(seed),
      decision_phase_(config.phase),
      phase_rng_(make_rng(seed, RngComponent::decision_phase)),
      config_(config),
      decay_(config.decay),
      clause_sat_workspace_(repr, num_vars),
      span_(config.span),
      intersection_(repr, num_vars),
      intersection_spare_rows_(repr, num_vars + 1),
      clause_equation_rng_(make_rng(seed, RngComponent::csids_equation)),
      saved_trails_(repr, num_vars, config.phase_saving) {
    assert(config.decay >= 0.0 && config.decay <= 1.0 && "CSIDS requires 0 <= decay <= 1");
    assert(config.focused_decay >= 0.0 && config.focused_decay <= 1.0 &&
           "CSIDS requires 0 <= focused decay <= 1");
    assert(config.initial_activity >= 0.0 && "CSIDS requires a non-negative A0");
    assert(config.bump >= 0.0 && "CSIDS requires a non-negative bump");
    assert(config.intersection_depth >= 1 && "CSIDS requires an intersection depth of at least 1");
    reinitialize();
}

void Csids::reinitialize() {
    phase_rng_ = make_rng(base_seed_, RngComponent::decision_phase);
    clause_equation_rng_ = make_rng(base_seed_, RngComponent::csids_equation);
    counters_ = {};
    heap_.reset();
    for (ClauseId id = 0; id < num_clauses_; ++id) {
        heap_.set_activity(id, 0.0);
        heap_.insert(id);
    }
    scale_ = 1.0;
    last_decision_clause_.reset();
    pending_learned_priority_.reset();
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

    while (!heap_.empty()) {
        const ClauseId id = heap_.peek_max();
        if (id >= clauses.size() || clause_active[id] == 0) {
            // Stale: naming a clause since deleted or made inactive. Neither is
            // ever undone, so the id is no candidate again and the heap need not
            // carry it.
            heap_.extract_max();
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
        DecisionResult result =
            decide_from_intersection(id, trail_span, clauses, clause_active, known);
        if (result.equation.has_value()) {
            last_decision_clause_ = id;
            ++level_;
        }
        return result;
    }

    last_decision_clause_.reset();
    return DecisionResult{};
}

DecisionResult Csids::decide_from_intersection(
    const ClauseId top_id, const RowSpace& trail_span, const std::vector<Clause>& clauses,
    const std::vector<std::uint8_t>& clause_active, const SatisfactionRecord& known) {
    MaxHeap& heap = heap_;
    assert(heap.peek_max() == top_id && "the top clause is not the heap's maximum");

    // Every reduced row is zero in the trail span's pivot columns, so a nonzero
    // combination of them lies outside the span; a clause is open only if `0 = 1`
    // is outside its span plus the trail's, so its negation lies outside too.
    // Under span^± the space also holds `0 = 1`, and a nonzero combination of
    // the other rows is a direction the trail does not settle.
    RowSpace& space = intersection_;
    EquationPool& spare_rows = intersection_spare_rows_;
    assert(space.rows() == 0 && "the last decision left rows in the intersection");
    // Rows in row-echelon order are stored as they are.
    for (const Equation& row : clause_sat_workspace_.reduced_clause()) {
        space.add_row(spare_rows.copy_of(row));
    }
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
    while (set_aside_ids_.size() < config_.intersection_depth && !heap.empty()) {
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
            continue;
        }
        std::vector<Equation> intersection =
            span_intersection(space, reduced_clause, spare_rows);
        if (intersection.empty()) {
            continue;
        }
        if (intersection.size() < dimension()) {
            changed_clause_ids_.push_back(id);
        }
        spare_rows.give_rows_of(space);
        for (Equation& row : intersection) {
            space.add_row(std::move(row));
        }
        if (plus_minus) {
            add_false_row_to_intersection();
        }
    }
    for (const ClauseId id : set_aside_ids_) {
        heap.insert(id);
    }

    if (propagating_id.has_value()) {
        spare_rows.give_rows_of(space);
        return DecisionResult{std::nullopt, propagating_id};
    }
    const bool consulted = consults_trails();
    if (consulted) {
        if (plus_minus) {
            // Phase saving takes V without `0 = 1`, and adds it back while the side is open.
            spare_rows.give(space.pop_row(space.rows() - 1));
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
    spare_rows.give_rows_of(space);
    ++counters_.decisions;
    if (holds_false || (span_ == CsidsSpan::plain && !consulted)) {
        const bool sided = decision_phase() != DecisionPhase::random;
        if (sided && holds_false) {
            orient_to_falsify(decision, clauses[top_id], trail_span);
        }
        if (phase_saving() && !consulted) {
            saved_trails_.note_unconsulted_decision();
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
    Equation side = intersection_spare_rows_.copy_of(eqn);
    const ReduceResult kind = clause_sat_workspace_.reduced_clause().partial_reduce(side);
    assert(!kind.is_leading() && "a decision under span^± is outside its top clause's span^±");
    if (kind.kind == ReduceResult::Kind::eq_false) {
        apply_phase(eqn, DecisionPhase::satisfy, phase_rng_);
    }
    intersection_spare_rows_.give(std::move(side));
}

void Csids::add_false_row_to_intersection() {
    Equation row = intersection_spare_rows_.take();
    row.set_eq_false();
    [[maybe_unused]] const std::optional<std::size_t> row_idx =
        intersection_.add_row_get_row_idx(std::move(row));
    assert(row_idx == std::optional<std::size_t>(intersection_.rows() - 1) &&
           "0 = 1 is not the intersection's last row");
}

void Csids::update_with_asserting_clause(const Clause&) {
    pending_learned_priority_ = config_.initial_activity * scale_;
    if (decay_ == 0.0) {
        // Growing the scale is the lazy form of decaying every activity. At zero
        // the scale would have to be infinite, so decay them directly.
        heap_.rescale(0.0);
    } else {
        scale_ *= (1.0 / decay_);
        // A bump rescales first unless conflicts go unbumped, which a decay of 0.5
        // takes 170 of to reach this.
        if (scale_ > 1e150) {
            heap_.rescale(1e-100);
            scale_ *= 1e-100;
        }
    }
}

void Csids::update_with_mode(SearchMode mode) {
    decay_ = mode == SearchMode::stable ? config_.decay : config_.focused_decay;
    saved_trails_.update_with_mode(mode);
}

ClauseStatus Csids::candidate_status(const ClauseId id, const Clause& clause,
                                     const RowSpace& trail_span, const SatisfactionRecord& known) {
    std::optional<std::size_t> level = known.satisfying_level(id);
    if (!level.has_value()) {
        const ClauseStatus status = clause_status_by_trail(trail_span, clause, clause_sat_workspace_);
        if (status != ClauseStatus::satisfied) {
            return status;
        }
        level = level_;
    }
    heap_.extract_max();
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
            heap_.insert(id);
        }
        satisfied_by_level_[discarded].clear();
    }
    level_ = level;
}

void Csids::update_with_new_clause(const Clause&, ClauseId id) {
    assert(id == num_clauses_ && "clause ids must be registered consecutively and once");
    heap_.set_activity(id, pending_learned_priority_.value_or(0.0));
    heap_.insert(id);
    num_clauses_ = id + 1;
    pending_learned_priority_.reset();
}

void Csids::update_with_conflict(const Trail& trail, std::span<const std::size_t> level_stamps) {
    if (phase_saving()) {
        saved_trails_.update_with_conflict(trail, level_stamps);
    }
}

void Csids::update_with_resolved_clause(ClauseId id) {
    assert(id < num_clauses_ && "a resolved clause was never registered");
    bump_activity(id);
}

void Csids::update_with_subsumed_clause(ClauseId subsumed_id, ClauseId replacement_id) {
    assert(subsumed_id < num_clauses_ && replacement_id < num_clauses_ &&
           "subsumption names an unregistered clause");
    const double inherited = clause_activity(subsumed_id);
    if (inherited > clause_activity(replacement_id)) {
        heap_.set_activity(replacement_id, inherited);
    }
}

void Csids::update_with_narrowed_clause(ClauseId id) {
    assert(id < num_clauses_ && "a narrowing names an unregistered clause");
    heap_.insert(id);
}

void Csids::apply_decision_phase(Equation& eqn) const {
    apply_phase(eqn, decision_phase_, phase_rng_);
}

void Csids::add_to_stats(RunStats& stats) const {
    stats.decisions = counters_.decisions;
    stats.phase_saving_open_sides = saved_trails_.counters().open_sides;
}

double Csids::clause_activity(ClauseId id) const {
    return heap_.activity(id);
}

std::optional<ClauseId> Csids::decision_clause_hint() const {
    return last_decision_clause_;
}

void Csids::update_with_substitution(const RowSpace& root_span,
                                     const IndexSet& dropped_col_idxs,
                                     const std::vector<Clause>& clauses,
                                     const std::vector<std::uint8_t>& clause_active) {
    num_clauses_ = static_cast<ClauseId>(clauses.size());
    saved_trails_.update_with_substitution(root_span, dropped_col_idxs);

    // The root trail is rebuilt, so no clause is known satisfied any more.
    for (std::vector<ClauseId>& ids : satisfied_by_level_) {
        for (const ClauseId id : ids) {
            heap_.insert(id);
        }
        ids.clear();
    }
    level_ = 0;

    if (last_decision_clause_.has_value()) {
        const ClauseId id = *last_decision_clause_;
        if (id >= clause_active.size() || clause_active[id] == 0) {
            last_decision_clause_.reset();
        }
    }
}

void Csids::update_with_renumbering(const CompactionTransform& transform) {
    assert(transform.old_num_vars() == num_vars() &&
           "the transform was built for a different active variable numbering");
    num_vars_ = transform.new_num_vars();
    clause_sat_workspace_.reconfigure(num_vars());
    intersection_ = RowSpace(repr(), num_vars());
    intersection_spare_rows_.clear(num_vars() + 1);
    saved_trails_.update_with_renumbering(transform);
}

} // namespace xorcle
