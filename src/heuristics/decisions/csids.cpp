#include "heuristics/decisions/csids.h"

#include <cassert>
#include <span>
#include <utility>

namespace xorcle {

void CsidsParams::bump_activity(ClauseId id) {
    activity_heap.increase_key(id, inc_amt);
    if (activity_heap.get_priority(id) > 1e100) {
        activity_heap.rescale(1e-100);
        inc_amt *= 1e-100;
    }
}

CsidsState::CsidsState(EquationRepr repr, std::size_t num_vars, const CsidsConfig& config)
    : initial_inc_amt(config.bump), repr(repr), num_vars(num_vars) {
    params.initial_activity = config.initial_activity;
    params.decay = config.decay;
    params.inc_amt = config.bump;
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
    params.activity_heap.reset();
    ensure_unit_eqns();
    for (ClauseId id = 0; id < num_clauses; ++id) {
        params.activity_heap.set_activity(id, 0.0);
        params.activity_heap.insert(id);
    }
    params.inc_amt = initial_inc_amt;
    last_decision_clause.reset();
    pending_learned_priority.reset();
}

Csids::Csids(EquationRepr repr, std::size_t num_vars, Seed seed, const CsidsConfig& config)
    : DecisionProcedure(repr, num_vars, seed),
      state_(repr, num_vars, config),
      clause_sat_workspace_(repr, num_vars),
      clause_equation_rng_(make_rng(seed, RngComponent::csids_equation)),
      candidate_(repr, num_vars),
      reduced_candidate_(repr, num_vars) {
    reinitialize();
}

void Csids::reinitialize() {
    reset_phase_rng();
    clause_equation_rng_ = make_rng(base_seed(), RngComponent::csids_equation);
    reset_stats();
    state_.reset_activity();
}

Equation Csids::make_decision(const RowSpace& trail_span, const std::vector<Clause>& clauses) {
    state_.ensure_unit_eqns();
    while (!state_.params.activity_heap.empty()) {
        const ClauseId id = state_.params.activity_heap.peek_max();
        if (id >= clauses.size()) {
            // A stale id left over from before a compaction shrank the database.
            state_.params.activity_heap.extract_max();
            continue;
        }
        const Clause& clause = clauses[id];
        if (clause_satisfied_by_trail(trail_span, clause, clause_sat_workspace_)) {
            state_.params.activity_heap.extract_max();
            continue;
        }
        state_.last_decision_clause = id;

        // The clause is not satisfied, which is exactly the precondition that
        // makes the rejection loop terminate.
        const bool sampled = sample_equation_from_clause(clause, trail_span, clause_equation_rng_,
                                                         candidate_, reduced_candidate_);
        assert(sampled && "Csids: sampled no legal equation from an unsatisfied clause");
        (void)sampled;

        record_decision(false);
        // The phase is applied to the *reduced* candidate, which is not the same
        // as applying it before reducing, so the reduced form is what is returned.
        apply_decision_phase(reduced_candidate_);
        return reduced_candidate_;
    }

    state_.last_decision_clause.reset();
    record_decision(true);
    return decide_from_basis(state_.unit_eqns, trail_span, reduced_candidate_);
}

void Csids::update_with_asserting_clause(const Clause&) {
    state_.pending_learned_priority = state_.params.initial_activity * state_.params.inc_amt;
    state_.params.inc_amt *= (1.0 / state_.params.decay);
    state_.params.activity_heap.rebuild();
}

void Csids::update_with_new_clause(const Clause&, ClauseId id) {
    if (id < state_.num_clauses) {
        return;
    }
    const std::optional<double> learned_priority = state_.pending_learned_priority;
    for (ClauseId next_id = state_.num_clauses; next_id <= id; ++next_id) {
        double priority = 0.0;
        if (learned_priority.has_value() && next_id == id) {
            priority = *learned_priority;
        }
        state_.params.activity_heap.set_activity(next_id, priority);
        state_.params.activity_heap.insert(next_id);
    }
    state_.num_clauses = id + 1;
    state_.pending_learned_priority.reset();
}

void Csids::update_with_resolved_clause(ClauseId id) {
    if (id < state_.num_clauses) {
        state_.params.bump_activity(id);
    }
}

std::optional<ClauseId> Csids::decision_clause_hint() const {
    return state_.last_decision_clause;
}

void Csids::update_with_compaction(const CompactionTransform& transform, const RowSpace&,
                                   const std::vector<Clause>& clauses,
                                   const std::vector<std::uint8_t>& clause_active) {
    state_.unit_eqns = project_basis_keep_order(state_.unit_eqns, transform, repr());

    set_num_vars(transform.new_num_vars());
    clause_sat_workspace_.reconfigure(num_vars());
    candidate_.resize(num_vars());
    candidate_.set_eq_true();
    reduced_candidate_.resize(num_vars());
    reduced_candidate_.set_eq_true();

    state_.num_vars = num_vars();
    state_.num_clauses = static_cast<ClauseId>(clauses.size());

    if (state_.last_decision_clause.has_value()) {
        const ClauseId id = *state_.last_decision_clause;
        if (id >= clause_active.size() || clause_active[id] == 0) {
            state_.last_decision_clause.reset();
        }
    }
}

} // namespace xorcle
