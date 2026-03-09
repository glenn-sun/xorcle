#include <algorithm>
#include <cassert>
#include <cstdint>
#include <sstream>
#include <stdexcept>

#include "heuristics/ClauseBasedDecisions.h"
#include "util/DecisionUtils.h"
#include "util/RandomUtils.h"

namespace heuristics {
void CVsidsParams::bump_activity(ClauseId id) {
    activity_heap.increase_key(id, inc_amt);
    if (activity_heap.get_priority(id) > 1e100) {
        activity_heap.rescale(1e-100);
        inc_amt *= 1e-100;
    }
}

CVsidsState::CVsidsState(std::size_t n, const CVsidsConfig& config)
    : num_vars(n) {
    params.learned_base = config.learned_base;
    params.decay = config.decay;
    initial_inc_amt = config.bump;
    params.inc_amt = config.bump;
}

void CVsidsState::ensure_unit_eqns() {
    if (unit_eqns.size() != num_vars) {
        unit_eqns.clear();
        unit_eqns.reserve(num_vars);
        for (std::size_t var = 0; var < num_vars; ++var) {
            unit_eqns.emplace_back(Equation::unit(num_vars + 1, var));
        }
    }
}

void CVsidsState::reset_activity() {
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

ClauseVsids::ClauseVsids(std::size_t n,
                         util::Seed seed,
                         const CVsidsConfig& config,
                         std::shared_ptr<CVsidsState> shared_state)
    : DecisionProcedure(n, seed),
      state(shared_state ? std::move(shared_state) : std::make_shared<CVsidsState>(n, config)),
      clause_sat_ws(n),
      clause_equation_rng(util::make_rng(seed, util::RngComponent::clause_vsids_equation)) {
    reinitialize();
}

void ClauseVsids::reinitialize() {
    reset_phase_rng();
    clause_equation_rng = util::make_rng(base_seed, util::RngComponent::clause_vsids_equation);
    reset_stats();
    state->reset_activity();
}

Equation ClauseVsids::make_decision(const RowSpace& trail_span, const std::vector<Clause>& clauses) {
    state->ensure_unit_eqns();
    Equation candidate{num_vars};
    Equation reduced_candidate{num_vars};
    while (!state->params.activity_heap.empty()) {
        ClauseId id = state->params.activity_heap.peek_max();
        if (id >= clauses.size()) {
            state->params.activity_heap.extract_max();
            continue;
        }
        const Clause& clause = clauses[id];
        if (util::clause_satisfied_by_trail(trail_span, clause, clause_sat_ws)) {
            state->params.activity_heap.extract_max();
            continue;
        }
        state->last_decision_clause = id;
        if (util::sample_equation_from_clause(clause, trail_span, num_vars, clause_equation_rng,
                                              candidate, reduced_candidate)) {
            record_decision(false);
            return apply_phase(reduced_candidate);
        }

        std::ostringstream msg;
        msg << "ClauseVsids::make_decision sampled no legal equation from active clause " << id;
        throw std::logic_error(msg.str());
    }

    state->last_decision_clause.reset();
    record_decision(true);
    return util::decide_from_units(trail_span, state->unit_eqns, num_vars);
}

void ClauseVsids::update_with_asserting_clause(const Clause& /*clause*/) {
    state->pending_learned_priority = state->params.learned_base * state->params.inc_amt;
    state->params.inc_amt *= (1 / state->params.decay);
    state->params.activity_heap.rebuild();
}

void ClauseVsids::update_with_new_clause(const Clause& /*clause*/, ClauseId id) {
    if (id < state->num_clauses) {
        return;
    }
    const std::optional<double> learned_priority = state->pending_learned_priority;
    for (ClauseId next_id = state->num_clauses; next_id <= id; ++next_id) {
        double priority = 0.0;
        if (learned_priority.has_value() && next_id == id) {
            priority = *learned_priority;
        }
        state->params.activity_heap.set_activity(next_id, priority);
        state->params.activity_heap.insert(next_id);
    }
    state->num_clauses = id + 1;
    state->pending_learned_priority.reset();
}

void ClauseVsids::update_with_resolved_clause(ClauseId id) {
    if (id < state->num_clauses) {
        state->params.bump_activity(id);
    }
}

std::optional<ClauseId> ClauseVsids::decision_clause_hint() const {
    return state->last_decision_clause;
}

void ClauseVsids::update_with_compaction(const search::CompactionTransform& transform,
                                         const RowSpace& trail_span,
                                         const std::vector<Clause>& clauses,
                                         const std::vector<std::uint8_t>& clause_active) {
    (void)trail_span;
    num_vars = transform.new_num_vars;
    clause_sat_ws.reconfigure(num_vars);

    state->num_vars = num_vars;
    state->unit_eqns = util::project_basis_keep_order(state->unit_eqns, transform);
    state->num_clauses = static_cast<ClauseId>(clauses.size());

    if (state->last_decision_clause.has_value()) {
        ClauseId id = *state->last_decision_clause;
        if (id >= clause_active.size() || clause_active[id] == 0) {
            state->last_decision_clause.reset();
        }
    }
}
} // namespace heuristics
