#include "heuristics/decisions/csids.h"

#include <cassert>
#include <chrono>
#include <optional>
#include <span>
#include <utility>

namespace xorcle {

void CsidsActivity::bump_activity(ClauseId id) {
    activity_heap.increase_key(id, inc_amt);
    if (activity_heap.get_priority(id) > 1e100) {
        activity_heap.rescale(1e-100);
        inc_amt *= 1e-100;
    }
}

CsidsState::CsidsState(EquationRepr repr, std::size_t num_vars, const CsidsConfig& config)
    : initial_inc_amt(config.bump), repr(repr), num_vars(num_vars) {
    assert(config.decay >= 0.0 && config.decay <= 1.0 && "CSIDS requires 0 <= decay <= 1");
    assert(config.initial_activity >= 0.0 && "CSIDS requires a non-negative A0");
    assert(config.bump >= 0.0 && "CSIDS requires a non-negative bump");
    assert(config.intersection_depth >= 1 && "CSIDS requires an intersection depth of at least 1");
    activity.initial_activity = config.initial_activity;
    activity.decay = config.decay;
    activity.inc_amt = config.bump;
}

void CsidsState::ensure_unit_eqns() {
    if (unit_eqns.size() == num_vars) {
        return;
    }
    unit_eqns.clear();
    unit_eqns.reserve(num_vars);
    for (std::size_t var = 0; var < num_vars; ++var) {
        unit_eqns.push_back(
            Equation::from_bits(repr, num_vars, std::span<const std::size_t>(&var, 1)));
    }
}

void CsidsState::reset_activity() {
    activity.activity_heap.reset();
    ensure_unit_eqns();
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
      state_(repr, num_vars, config),
      clause_sat_workspace_(repr, num_vars),
      intersection_depth_(config.intersection_depth),
      clause_equation_rng_(make_rng(seed, RngComponent::csids_equation)) {
    reinitialize();
}

void Csids::reinitialize() {
    reset_phase_rng();
    clause_equation_rng_ = make_rng(base_seed(), RngComponent::csids_equation);
    reset_stats();
    intersection_stats_ = {};
    state_.reset_activity();
    satisfied_by_level_.clear();
    level_ = 0;
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

    state_.ensure_unit_eqns();
    while (!state_.activity.activity_heap.empty()) {
        const ClauseId id = state_.activity.activity_heap.peek_max();
        if (id >= clauses.size() || clause_active[id] == 0) {
            // Stale: left over from before a compaction shrank the database, or
            // naming a clause since deleted. Neither is ever undone, so the id
            // is no candidate again and the heap need not carry it.
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
        if (intersection_depth_ > 1) {
            const auto start = std::chrono::steady_clock::now();
            std::optional<DecisionResult> result =
                decide_from_intersection(id, trail_span, clauses, clause_active, known);
            const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start;
            intersection_stats_.time_s += elapsed.count();
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

        record_decision(false);
        apply_decision_phase(sample.reduced);
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
    RowSpace space = clause_sat_workspace_.reduced_clause();
    const std::size_t dimension_before = space.rows();

    // Open clauses are set aside to surface the ones below, and all go back.
    set_aside_ids_.clear();
    set_aside_ids_.push_back(top_id);
    heap.extract_max();
    std::optional<ClauseId> propagating_id;
    std::size_t intersected_clauses = 0;
    while (set_aside_ids_.size() < intersection_depth_ && !heap.empty()) {
        const ClauseId id = heap.peek_max();
        if (id >= clauses.size() || clause_active[id] == 0) {
            heap.extract_max();
            continue;
        }
        const ClauseStatus status = candidate_status(id, clauses[id], trail_span, known);
        if (status == ClauseStatus::satisfied) {
            ++intersection_stats_.passed_satisfied_clauses;
            continue;
        }
        if (status == ClauseStatus::propagating) {
            propagating_id = id;
            break;
        }
        set_aside_ids_.push_back(id);
        heap.extract_max();

        std::vector<Equation> intersection =
            span_intersection(space, clause_sat_workspace_.reduced_clause());
        if (intersection.empty()) {
            continue;
        }
        space = RowSpace(repr(), num_vars());
        for (Equation& row : intersection) {
            space.add_row(std::move(row));
        }
        ++intersected_clauses;
    }
    for (const ClauseId id : set_aside_ids_) {
        heap.insert(id);
    }

    if (propagating_id.has_value()) {
        ++intersection_stats_.propagating_clauses;
        return DecisionResult{std::nullopt, propagating_id};
    }
    if (intersected_clauses == 0) {
        return std::nullopt;
    }
    ++intersection_stats_.decisions;
    intersection_stats_.intersected_clauses += intersected_clauses;
    intersection_stats_.dimension_before += dimension_before;
    intersection_stats_.dimension_after += space.rows();

    Equation decision = Equation::eq_true(repr(), num_vars());
    random_nonzero_coefficients(space.rows(), clause_equation_rng_)
        .for_each_set([&](std::size_t row_idx) { decision.plus_assign(space.row(row_idx)); });
#ifndef NDEBUG
    Equation reduced = decision;
    assert(trail_span.partial_reduce(reduced).is_leading() &&
           "a decision from an intersection is not legal");
#endif
    record_decision(false);
    apply_decision_phase(decision);
    return DecisionResult{std::move(decision), std::nullopt};
}

void Csids::update_with_asserting_clause(const Clause&) {
    state_.pending_learned_priority = state_.activity.initial_activity * state_.activity.inc_amt;
    if (state_.activity.decay == 0.0) {
        // Growing the increment is the lazy form of decaying every activity. At
        // zero the increment would have to be infinite, so decay them directly.
        state_.activity.activity_heap.rescale(0.0);
    } else {
        state_.activity.inc_amt *= (1.0 / state_.activity.decay);
    }
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

DecisionProcedure::DecisionStats Csids::decision_stats() const {
    DecisionStats stats = DecisionProcedure::decision_stats();
    stats.intersections = intersection_stats_;
    return stats;
}

double Csids::clause_activity(ClauseId id) const {
    return state_.activity.activity_heap.get_priority(id);
}

std::optional<ClauseId> Csids::decision_clause_hint() const {
    return state_.last_decision_clause;
}

std::size_t Csids::reusable_levels(
    const RowSpace& trail_span, const std::vector<Clause>& clauses,
    const std::vector<std::uint8_t>& clause_active,
    const std::vector<std::optional<ClauseId>>& decision_clauses, const SatisfactionRecord& known) {
    assert(clause_active.size() == clauses.size() && "clause active flags are desynchronized");

    // The same walk `make_decision()` makes; what it sets aside, the restart's
    // backjump puts back.
    std::optional<ClauseId> next;
    while (!state_.activity.activity_heap.empty()) {
        const ClauseId id = state_.activity.activity_heap.peek_max();
        if (id >= clauses.size() || clause_active[id] == 0) {
            state_.activity.activity_heap.extract_max();
            continue;
        }
        if (candidate_status(id, clauses[id], trail_span, known) != ClauseStatus::satisfied) {
            next = id;
            break;
        }
    }

    // Nothing left to decide from, so every level is one the fallback basis
    // would have to supply again.
    if (!next.has_value()) {
        return 0;
    }

    const double next_activity = state_.activity.activity_heap.get_priority(*next);
    std::size_t levels = 0;
    while (levels < decision_clauses.size()) {
        const std::optional<ClauseId>& id = decision_clauses[levels];
        if (!id.has_value() || *id >= clause_active.size() || clause_active[*id] == 0 ||
            state_.activity.activity_heap.get_priority(*id) <= next_activity) {
            break;
        }
        ++levels;
    }
    return levels;
}

void Csids::update_with_compaction(const CompactionTransform& transform, const RowSpace&,
                                   const std::vector<Clause>& clauses,
                                   const std::vector<std::uint8_t>& clause_active) {
    assert(transform.old_num_vars() == num_vars() &&
           "the transform was built for a different active variable numbering");
    state_.unit_eqns = compact_basis_keep_order(state_.unit_eqns, transform, repr());

    set_num_vars(transform.new_num_vars());
    clause_sat_workspace_.reconfigure(num_vars());

    state_.num_vars = num_vars();
    state_.num_clauses = static_cast<ClauseId>(clauses.size());

    // Every clause is rewritten, and the root trail rebuilt, so none is known
    // satisfied any more.
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

} // namespace xorcle
