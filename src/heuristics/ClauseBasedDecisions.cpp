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
        if (util::clause_satisfied_by_trail(trail_span, clause, num_vars)) {
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

void BerkminDecision::reinitialize() {
    reset_stats();
    conflict_clauses.clear();
    pending_conflict_clause = false;
    last_decision_clause.reset();
    ensure_unit_eqns_impl(unit_eqns, num_vars);
}

Equation BerkminDecision::make_decision(const RowSpace& trail_span, const std::vector<Clause>& clauses) {
    ensure_unit_eqns_impl(unit_eqns, num_vars);
    Equation candidate{num_vars};
    Equation reduced{num_vars};
    for (ClauseId id : conflict_clauses) {
        if (id >= clauses.size()) {
            continue;
        }
        const Clause& clause = clauses[id];
        if (util::clause_satisfied_by_trail(trail_span, clause, num_vars)) {
            continue;
        }
        last_decision_clause = id;
        if (util::sample_equation_from_clause(clause, trail_span, num_vars, candidate, reduced)) {
            record_decision(false);
            return apply_phase(reduced);
        }
    }
    last_decision_clause.reset();
    record_decision(true);
    return util::decide_from_units(trail_span, unit_eqns, num_vars);
}

void BerkminDecision::update_with_asserting_clause(const Clause& /*clause*/) {
    pending_conflict_clause = true;
}

void BerkminDecision::update_with_new_clause(const Clause& /*clause*/, ClauseId id) {
    if (!pending_conflict_clause) {
        return;
    }
    conflict_clauses.push_front(id);
    pending_conflict_clause = false;
}

std::optional<ClauseId> BerkminDecision::decision_clause_hint() const {
    return last_decision_clause;
}

void CBHDecision::ensure_unit_eqns() {
    ensure_unit_eqns_impl(unit_eqns, num_vars);
}

void CBHDecision::reinitialize() {
    reset_stats();
    clause_list.clear();
    positions.clear();
    list_built = false;
    current = clause_list.end();
    pending_responsible.clear();
    pending_mark.clear();
    pending_conflict_clause = false;
    pending_conflict_clause_id.reset();
    last_decision_clause.reset();
    ensure_unit_eqns();
}

void CBHDecision::build_initial_list(const std::vector<Clause>& clauses) {
    clause_list.clear();
    positions.clear();
    positions.resize(clauses.size(), clause_list.end());
    list_built = true;

    if (clauses.empty()) {
        current = clause_list.end();
        return;
    }

    const std::size_t nvars = num_vars;
    std::vector<std::vector<ClauseId>> var_to_clauses(nvars);
    std::vector<std::vector<std::size_t>> clause_vars(clauses.size());
    std::vector<int> igs(nvars, 0);

    for (ClauseId id = 0; id < clauses.size(); ++id) {
        std::vector<char> seen(nvars, false);
        const Clause& clause = clauses[id];
        for (std::size_t r = 0; r < clause.rows(); ++r) {
            clause.row(r).if_set_call([&](std::size_t idx) {
                if (idx >= nvars || seen[idx]) {
                    return;
                }
                seen[idx] = true;
                clause_vars[id].push_back(idx);
                igs[idx] += 1;
                var_to_clauses[idx].push_back(id);
            });
        }
    }

    MaxHeap<std::size_t> heap;
    for (std::size_t v = 0; v < nvars; ++v) {
        heap.set_activity(v, static_cast<double>(igs[v]));
        heap.insert(v);
    }

    std::vector<char> picked(nvars, false);
    std::vector<char> clause_added(clauses.size(), false);
    std::size_t picked_count = 0;

    while (picked_count < nvars) {
        std::optional<std::size_t> maybe_var = heap.extract_max();
        if (!maybe_var.has_value()) {
            break;
        }
        std::size_t var = maybe_var.value();
        if (picked[var]) {
            continue;
        }
        picked[var] = true;
        ++picked_count;

        for (ClauseId id : var_to_clauses[var]) {
            if (clause_added[id]) {
                continue;
            }
            clause_added[id] = true;
            clause_list.push_back(id);
            auto it = std::prev(clause_list.end());
            positions[id] = it;
            for (std::size_t v : clause_vars[id]) {
                heap.increase_key(v, 1.0);
            }
        }
    }

    for (ClauseId id = 0; id < clauses.size(); ++id) {
        if (!clause_added[id]) {
            clause_list.push_back(id);
            positions[id] = std::prev(clause_list.end());
        }
    }

    current = clause_list.begin();

    if (pending_conflict_clause_id.has_value()) {
        apply_pending_reorder(pending_conflict_clause_id.value());
        pending_conflict_clause_id.reset();
    }
}

void CBHDecision::apply_pending_reorder(ClauseId conflict_id) {
    if (!list_built) {
        return;
    }
    if (positions.size() <= conflict_id) {
        positions.resize(conflict_id + 1, clause_list.end());
    }

    if (positions[conflict_id] == clause_list.end()) {
        clause_list.push_front(conflict_id);
        positions[conflict_id] = clause_list.begin();
    } else {
        clause_list.splice(clause_list.begin(), clause_list, positions[conflict_id]);
        positions[conflict_id] = clause_list.begin();
    }

    auto insert_pos = std::next(positions[conflict_id]);
    for (ClauseId id : pending_responsible) {
        if (id == conflict_id) {
            if (id < pending_mark.size()) {
                pending_mark[id] = false;
            }
            continue;
        }
        if (positions.size() <= id) {
            positions.resize(id + 1, clause_list.end());
        }
        if (positions[id] == clause_list.end()) {
            positions[id] = clause_list.insert(insert_pos, id);
            insert_pos = std::next(positions[id]);
        } else {
            clause_list.splice(insert_pos, clause_list, positions[id]);
            insert_pos = std::next(positions[id]);
        }
        if (id < pending_mark.size()) {
            pending_mark[id] = false;
        }
    }
    pending_responsible.clear();
    current = clause_list.begin();
}

Equation CBHDecision::make_decision(const RowSpace& trail_span, const std::vector<Clause>& clauses) {
    ensure_unit_eqns();
    if (!list_built) {
        build_initial_list(clauses);
    }
    if (clause_list.empty()) {
        record_decision(true);
        return util::decide_from_units(trail_span, unit_eqns, num_vars);
    }

    Equation candidate{num_vars};
    Equation reduced{num_vars};

    auto it = (current == clause_list.end()) ? clause_list.begin() : current;
    for (; it != clause_list.end(); ++it) {
        ClauseId id = *it;
        if (id >= clauses.size()) {
            continue;
        }
        const Clause& clause = clauses[id];
        if (util::clause_satisfied_by_trail(trail_span, clause, num_vars)) {
            continue;
        }
        last_decision_clause = id;
        current = it;
        if (util::sample_equation_from_clause(clause, trail_span, num_vars, candidate, reduced)) {
            record_decision(false);
            return apply_phase(reduced);
        }
    }

    last_decision_clause.reset();
    record_decision(true);
    return util::decide_from_units(trail_span, unit_eqns, num_vars);
}

void CBHDecision::update_with_asserting_clause(const Clause& /*clause*/) {
    pending_conflict_clause = true;
}

void CBHDecision::update_with_resolved_clause(ClauseId id) {
    if (pending_mark.size() <= id) {
        pending_mark.resize(id + 1, false);
    }
    if (!pending_mark[id]) {
        pending_mark[id] = true;
        pending_responsible.push_back(id);
    }
}

void CBHDecision::update_with_new_clause(const Clause& /*clause*/, ClauseId id) {
    if (!pending_conflict_clause) {
        return;
    }
    pending_conflict_clause = false;
    if (!list_built) {
        pending_conflict_clause_id = id;
        return;
    }
    apply_pending_reorder(id);
}

std::optional<ClauseId> CBHDecision::decision_clause_hint() const {
    return last_decision_clause;
}

void CVsidsParams::bump_activity(ClauseId id) {
    activity_heap.increase_key(id, inc_amt);
    if (activity_heap.get_priority(id) > 1e100) {
        activity_heap.rescale(1e-100);
        inc_amt *= 1e-100;
    }
}

CVsidsState::CVsidsState(std::size_t n)
    : eqn_false(Equation::unit(n + 1, n)), num_vars(n) {}

void CVsidsState::ensure_unit_eqns() {
    if (unit_eqns.size() != num_vars) {
        unit_eqns.clear();
        unit_eqns.reserve(num_vars);
        for (std::size_t i = 0; i < num_vars; i++) {
            unit_eqns.emplace_back(Equation::unit(num_vars + 1, i));
        }
    }
}

void CVsidsState::reset_activity() {
    params.activity_heap.reset();
    ensure_unit_eqns();
    for (ClauseId id = 0; id < num_clauses; ++id) {
        params.activity_heap.set_activity(id, 0);
        params.activity_heap.insert(id);
    }
    params.inc_amt = 1;
    last_decision_clause.reset();
}

CVsidsDecision::CVsidsDecision(std::size_t n, std::shared_ptr<CVsidsState> shared_state)
    : DecisionProcedure(n), state(shared_state ? std::move(shared_state) : std::make_shared<CVsidsState>(n)) {
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
        if (util::clause_satisfied_by_trail(trail_span, clause, num_vars)) {
            state->params.activity_heap.extract_max();
            continue;
        }
        state->last_decision_clause = id;
        if (util::sample_equation_from_clause(clause, trail_span, num_vars, candidate, reduced)) {
            ++decisions;
            return apply_phase(reduced);
        }
    }

    state->last_decision_clause.reset();
    ++decisions;
    ++fallback_decisions;
    return util::decide_from_units(trail_span, state->unit_eqns, num_vars);
}

void CVsidsDecision::update_with_asserting_clause(const Clause& /*clause*/) {
    state->params.inc_amt *= (1 / state->params.decay);
    state->params.activity_heap.rebuild();
}

void CVsidsDecision::update_with_new_clause(const Clause& /*clause*/, ClauseId id) {
    if (id < state->num_clauses) {
        return;
    }
    for (ClauseId cur = state->num_clauses; cur <= id; ++cur) {
        state->params.activity_heap.set_activity(cur, 0);
        state->params.activity_heap.insert(cur);
    }
    state->num_clauses = id + 1;
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
