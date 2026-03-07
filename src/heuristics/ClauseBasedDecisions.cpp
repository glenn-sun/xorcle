#include <algorithm>
#include <cassert>
#include <numeric>

#include "heuristics/ClauseBasedDecisions.h"
#include "heuristics/DecisionProcedure.h"
#include "util/DecisionUtils.h"
#include "util/RandomUtils.h"

namespace heuristics {
namespace {
/// @brief Ensure a unit equation basis is initialized to size num_vars.
void ensure_unit_eqns_impl(std::vector<Equation>& unit_eqns, std::size_t num_vars) {
    if (unit_eqns.size() != num_vars) {
        unit_eqns.clear();
        unit_eqns.reserve(num_vars);
        for (std::size_t i = 0; i < num_vars; i++) {
            unit_eqns.emplace_back(Equation::unit(num_vars + 1, i));
        }
    }
}
} // namespace

void RandomClauseSum::reinitialize() {
    reset_stats();
    decision_basis.clear();
    for (std::size_t i = 0; i < num_vars; i++) {
        decision_basis.emplace_back(Equation::unit(num_vars + 1, i));
    }
    clause_perm.clear();
}

Equation RandomClauseSum::make_decision(const RowSpace& trail_span, const std::vector<Clause>& clauses) {
    if (clauses.empty()) {
        record_decision(true);
        return util::decide_from_basis(decision_basis, trail_span, num_vars);
    }

    Equation candidate{num_vars};
    Equation reduced{num_vars};
    clause_perm.resize(clauses.size());
    std::iota(clause_perm.begin(), clause_perm.end(), 0);
    std::shuffle(clause_perm.begin(), clause_perm.end(), rng);

    for (ClauseId id : clause_perm) {
        const Clause& clause = clauses[id];
        if (util::clause_satisfied_by_trail(trail_span, clause, clause_sat_ws)) {
            continue;
        }

        if (util::sample_equation_from_clause(clause, trail_span, num_vars, candidate, reduced)) {
            record_decision(false);
            return apply_phase(candidate);
        }
    }

    record_decision(true);
    return util::decide_from_basis(decision_basis, trail_span, num_vars);
}

Equation VsidsClause::make_decision(const RowSpace& trail_span, const std::vector<Clause>& clauses) {
    while (true) {
        std::optional<Equation> maybe_eqn = vsids_params.activity_heap.extract_max();
        if (!maybe_eqn.has_value()) {
            reinitialize();
            maybe_eqn = vsids_params.activity_heap.extract_max();
        }

        std::size_t k = (*maybe_eqn).first_set();

        std::size_t start = util::random_int(clauses.size());
        for (std::size_t i = 0; i < clauses.size(); ++i) {
            std::size_t j = (start + i) % clauses.size();
            if (clauses[j].get_pivot_cols()[k]) {
                assert((*maybe_eqn).size() == clauses[j].row(clauses[j].get_col_to_row(k)).size());
                (*maybe_eqn).unsafe_copy(clauses[j].row(clauses[j].get_col_to_row(k)));
                const std::size_t constant_col = maybe_eqn->size() - 1;
                std::size_t f = trail_span.partial_reduce(*maybe_eqn);
                if ((f != bit::vector<>::npos) && (f != constant_col)) {
                    record_decision(false);
                    return apply_phase(*maybe_eqn);
                };
            }
        }
    }
}

void CVsidsParams::bump_activity(ClauseId id) {
    activity_heap.increase_key(id, inc_amt);
    if (activity_heap.get_priority(id) > 1e100) {
        activity_heap.rescale(1e-100);
        inc_amt *= 1e-100;
    }
}

CVsidsState::CVsidsState(std::size_t n, const CVsidsConfig& config)
    : eqn_false(Equation::unit(n + 1, n)), num_vars(n) {
    params.learned_base = config.learned_base;
    params.decay = config.decay;
    initial_inc_amt = config.bump;
    params.inc_amt = config.bump;
}

void CVsidsState::ensure_unit_eqns() {
    ensure_unit_eqns_impl(unit_eqns, num_vars);
}

void CVsidsState::reset_activity() {
    params.activity_heap.reset();
    ensure_unit_eqns();
    for (ClauseId id = 0; id < num_clauses; ++id) {
        params.activity_heap.set_activity(id, 0);
        params.activity_heap.insert(id);
    }
    params.inc_amt = initial_inc_amt;
    last_decision_clause.reset();
    pending_learned_priority.reset();
}

CVsidsDecision::CVsidsDecision(std::size_t n,
                               const CVsidsConfig& config,
                               std::shared_ptr<CVsidsState> shared_state)
    : DecisionProcedure(n),
      state(shared_state ? std::move(shared_state) : std::make_shared<CVsidsState>(n, config)),
      clause_sat_ws(n) {
    reinitialize();
}

void CVsidsDecision::reinitialize() {
    reset_stats();
    state->reset_activity();
}

Equation CVsidsDecision::make_decision(const RowSpace& trail_span, const std::vector<Clause>& clauses) {
    state->ensure_unit_eqns();
    Equation candidate{num_vars};
    Equation reduced{num_vars};
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
        if (util::sample_equation_from_clause(clause, trail_span, num_vars, candidate, reduced)) {
            record_decision(false);
            return apply_phase(reduced);
        }
    }

    state->last_decision_clause.reset();
    record_decision(true);
    return util::decide_from_units(trail_span, state->unit_eqns, num_vars);
}

void CVsidsDecision::update_with_asserting_clause(const Clause& /*clause*/) {
    state->pending_learned_priority = state->params.learned_base * state->params.inc_amt;
    state->params.inc_amt *= (1 / state->params.decay);
    state->params.activity_heap.rebuild();
}

void CVsidsDecision::update_with_new_clause(const Clause& /*clause*/, ClauseId id) {
    if (id < state->num_clauses) {
        return;
    }
    const std::optional<double> learned_priority = state->pending_learned_priority;
    for (ClauseId cur = state->num_clauses; cur <= id; ++cur) {
        double priority = 0.0;
        if (learned_priority.has_value() && cur == id) {
            priority = *learned_priority;
        }
        state->params.activity_heap.set_activity(cur, priority);
        state->params.activity_heap.insert(cur);
    }
    state->num_clauses = id + 1;
    state->pending_learned_priority.reset();
}

void CVsidsDecision::update_with_resolved_clause(ClauseId id) {
    if (id < state->num_clauses) {
        state->params.bump_activity(id);
    }
}

std::optional<ClauseId> CVsidsDecision::decision_clause_hint() const {
    return state->last_decision_clause;
}
} // namespace heuristics
