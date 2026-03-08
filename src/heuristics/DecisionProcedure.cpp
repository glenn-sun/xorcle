#include <cassert>
#include <unordered_map>
#include <unordered_set>

#include "heuristics/DecisionProcedure.h"
#include "util/DecisionUtils.h"
#include "util/RandomUtils.h"

namespace heuristics {
namespace {

std::vector<Equation> project_basis_keep_order(const std::vector<Equation>& basis,
                                               const search::CompactionTransform& transform) {
    std::vector<Equation> out;
    out.reserve(transform.new_num_vars);
    std::vector<std::uint8_t> seen(transform.new_num_vars, 0);
    for (const Equation& eqn : basis) {
        Equation projected = transform.project_equation(eqn);
        std::size_t f = projected.first_set();
        if (f >= transform.new_num_vars) {
            continue;
        }
        if (seen[f]) {
            continue;
        }
        seen[f] = 1;
        out.push_back(std::move(projected));
    }
    for (std::size_t i = 0; i < transform.new_num_vars; ++i) {
        if (!seen[i]) {
            out.push_back(Equation::unit(transform.new_num_vars + 1, i));
        }
    }
    return out;
}

void remap_vsids_heap(MaxHeap<Equation>& heap, const search::CompactionTransform& transform) {
    using Snapshot = typename MaxHeap<Equation>::StateSnapshot;
    Snapshot old = heap.snapshot();
    Snapshot next;

    std::unordered_map<Equation, double> activity_accum;
    activity_accum.reserve(old.activity.size());
    for (const auto& [eqn, score] : old.activity) {
        Equation projected = transform.project_equation(eqn);
        std::size_t f = projected.first_set();
        if (f == bit::vector<>::npos || f == transform.new_num_vars) {
            continue;
        }
        activity_accum[projected] += score;
    }
    next.activity.reserve(activity_accum.size());
    for (const auto& [eqn, score] : activity_accum) {
        next.activity.emplace(eqn, score);
    }

    std::unordered_set<Equation> seen_heap;
    seen_heap.reserve(old.heap.size());
    for (const Equation& eqn : old.heap) {
        Equation projected = transform.project_equation(eqn);
        if (next.activity.find(projected) == next.activity.end()) {
            continue;
        }
        if (seen_heap.insert(projected).second) {
            next.heap.push_back(std::move(projected));
        }
    }

    std::unordered_set<Equation> seen_popped;
    seen_popped.reserve(old.popped.size());
    for (const Equation& eqn : old.popped) {
        Equation projected = transform.project_equation(eqn);
        if (next.activity.find(projected) == next.activity.end()) {
            continue;
        }
        if (seen_popped.insert(projected).second) {
            next.popped.push_back(std::move(projected));
        }
    }

    heap.load_snapshot(std::move(next));
}

} // namespace

Equation DecisionProcedure::apply_phase(Equation eqn) const {
    switch (decision_phase) {
        case DecisionPhase::unsat:
            return eqn;
        case DecisionPhase::sat:
            eqn.flip(num_vars);
            return eqn;
        case DecisionPhase::random:
            if (util::random_int(2) == 1) {
                eqn.flip(num_vars);
            }
            return eqn;
    }
    return eqn;
}

void FixedUnit::reinitialize() {
    reset_stats();
    decision_basis.clear();
    for (std::size_t i = 0; i < num_vars; i++) {
        decision_basis.emplace_back(Equation::unit(num_vars + 1, i));
    }
}

Equation FixedUnit::make_decision(const RowSpace& trail_span, const std::vector<Clause>&) { 
    record_decision(false);
    return util::decide_from_basis(decision_basis, trail_span, num_vars);
}

void FixedUnit::update_with_compaction(const search::CompactionTransform& transform,
                                       const RowSpace& trail_span,
                                       const std::vector<Clause>& clauses,
                                       const std::vector<std::uint8_t>& clause_active) {
    (void)trail_span;
    (void)clauses;
    (void)clause_active;
    num_vars = transform.new_num_vars;
    decision_basis = project_basis_keep_order(decision_basis, transform);
}

void VsidsParams::bump_activity(const Equation& eqn) {
    activity_heap.increase_key(eqn, inc_amt);
    if (activity_heap.get_priority(eqn) > 1e100) {
        activity_heap.rescale(1e-100);
        inc_amt *= 1e-100;
    }
}

void VsidsUnit::reinitialize() {
    reset_stats();
    vsids_params.activity_heap.reset();
    if (unit_eqns.size() != num_vars) {
        unit_eqns.clear();
        unit_eqns.reserve(num_vars);
        for (std::size_t i = 0; i < num_vars; i++) {
            unit_eqns.emplace_back(Equation::unit(num_vars + 1, i));
        }
    }
    for (std::size_t i = 0; i < num_vars; i++) {
        const Equation& unit = unit_eqns[i];
        vsids_params.activity_heap.set_activity(unit, 0);
        vsids_params.activity_heap.insert(unit);
    }
    vsids_params.inc_amt = 1;
}

Equation VsidsUnit::make_decision(const RowSpace& trail_span, const std::vector<Clause>&) { 
    while (true) {
        std::optional<Equation> maybe_eqn = vsids_params.activity_heap.extract_max();
        if (!maybe_eqn.has_value()) {
            reinitialize();
            maybe_eqn = vsids_params.activity_heap.extract_max();
        }
        std::size_t f = trail_span.partial_reduce(*maybe_eqn);
        if ((f != bit::vector<>::npos) && (f != num_vars)) {
            record_decision(false);
            return *maybe_eqn;
        };
    }
}

void VsidsUnit::update_with_critical_equation(const Equation& eqn) {
    std::size_t leading_term = eqn.first_set();
    assert(leading_term < unit_eqns.size());
    vsids_params.bump_activity(unit_eqns[leading_term]);
}

void VsidsUnit::update_with_asserting_clause(const Clause& clause) {
    // Bump vars in final clause
    for (std::size_t i = 0; i < clause.rows(); ++i) {
        std::size_t var = clause.row(i).first_set();
        assert(var < unit_eqns.size());
        vsids_params.bump_activity(unit_eqns[var]);
    }

    // Decay activity by increasing increment
    vsids_params.inc_amt *= (1 / vsids_params.decay);

    vsids_params.activity_heap.rebuild();
}

void VsidsUnit::update_with_compaction(const search::CompactionTransform& transform,
                                       const RowSpace& trail_span,
                                       const std::vector<Clause>& clauses,
                                       const std::vector<std::uint8_t>& clause_active) {
    (void)trail_span;
    (void)clauses;
    (void)clause_active;
    num_vars = transform.new_num_vars;
    eqn_false = Equation::unit(num_vars + 1, num_vars);
    unit_eqns = project_basis_keep_order(unit_eqns, transform);
    remap_vsids_heap(vsids_params.activity_heap, transform);
    if (vsids_params.activity_heap.empty()) {
        for (std::size_t i = 0; i < num_vars; ++i) {
            const Equation& unit = unit_eqns[i];
            if (vsids_params.activity_heap.get_priority(unit) == 0.0) {
                vsids_params.activity_heap.set_activity(unit, 0.0);
            }
            vsids_params.activity_heap.insert(unit);
        }
    }
}
} // namespace heuristics
