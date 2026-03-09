#include <cassert>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

#include "heuristics/DecisionProcedure.h"
#include "util/DecisionUtils.h"
#include "util/RandomUtils.h"

namespace heuristics {
namespace {
void remap_vsids_heap(MaxHeap<Equation>& heap, const search::CompactionTransform& transform) {
    using Snapshot = typename MaxHeap<Equation>::StateSnapshot;
    Snapshot old_snapshot = heap.snapshot();
    Snapshot next_snapshot;

    std::unordered_map<Equation, double> activity_by_projected_eqn;
    activity_by_projected_eqn.reserve(old_snapshot.activity.size());
    for (const auto& [eqn, score] : old_snapshot.activity) {
        Equation projected_eqn = transform.project_equation(eqn);
        std::size_t leading_col = projected_eqn.first_set();
        if (leading_col == bit::vector<>::npos || leading_col == transform.new_num_vars) {
            continue;
        }
        activity_by_projected_eqn[projected_eqn] += score;
    }
    next_snapshot.activity.reserve(activity_by_projected_eqn.size());
    for (const auto& [eqn, score] : activity_by_projected_eqn) {
        next_snapshot.activity.emplace(eqn, score);
    }

    std::unordered_set<Equation> seen_heap;
    seen_heap.reserve(old_snapshot.heap.size());
    for (const Equation& eqn : old_snapshot.heap) {
        Equation projected_eqn = transform.project_equation(eqn);
        if (next_snapshot.activity.find(projected_eqn) == next_snapshot.activity.end()) {
            continue;
        }
        if (seen_heap.insert(projected_eqn).second) {
            next_snapshot.heap.push_back(std::move(projected_eqn));
        }
    }

    std::unordered_set<Equation> seen_popped;
    seen_popped.reserve(old_snapshot.popped.size());
    for (const Equation& eqn : old_snapshot.popped) {
        Equation projected_eqn = transform.project_equation(eqn);
        if (next_snapshot.activity.find(projected_eqn) == next_snapshot.activity.end()) {
            continue;
        }
        if (seen_popped.insert(projected_eqn).second) {
            next_snapshot.popped.push_back(std::move(projected_eqn));
        }
    }

    heap.load_snapshot(std::move(next_snapshot));
}

} // namespace

void DecisionProcedure::reset_phase_rng() {
    phase_rng = util::make_rng(base_seed, util::RngComponent::decision_phase);
}

Equation DecisionProcedure::apply_phase(Equation eqn) const {
    switch (decision_phase) {
        case DecisionPhase::unsat:
            return eqn;
        case DecisionPhase::sat:
            eqn.flip(num_vars);
            return eqn;
        case DecisionPhase::random:
            if (util::random_int(phase_rng, 2) == 1) {
                eqn.flip(num_vars);
            }
            return eqn;
    }
    return eqn;
}

void FixedLiterals::reinitialize() {
    reset_phase_rng();
    reset_stats();
    decision_basis.clear();
    decision_basis.reserve(num_vars);
    for (std::size_t var = 0; var < num_vars; ++var) {
        decision_basis.emplace_back(Equation::unit(num_vars + 1, var));
    }
}

Equation FixedLiterals::make_decision(const RowSpace& trail_span, const std::vector<Clause>&) {
    record_decision(false);
    return util::decide_from_basis(decision_basis, trail_span, num_vars);
}

void FixedLiterals::update_with_compaction(const search::CompactionTransform& transform,
                                           const RowSpace& trail_span,
                                           const std::vector<Clause>& clauses,
                                           const std::vector<std::uint8_t>& clause_active) {
    (void)trail_span;
    (void)clauses;
    (void)clause_active;
    num_vars = transform.new_num_vars;
    decision_basis = util::project_basis_keep_order(decision_basis, transform);
}

void VsidsParams::bump_activity(const Equation& eqn) {
    activity_heap.increase_key(eqn, inc_amt);
    if (activity_heap.get_priority(eqn) > 1e100) {
        activity_heap.rescale(1e-100);
        inc_amt *= 1e-100;
    }
}

void VariableVsids::reinitialize() {
    reset_phase_rng();
    reset_stats();
    reset_decision_heap_for_fresh_run();
}

void VariableVsids::reset_decision_heap_for_fresh_run() {
    vsids_params.activity_heap.reset();
    if (unit_eqns.size() != num_vars) {
        unit_eqns.clear();
        unit_eqns.reserve(num_vars);
        for (std::size_t var = 0; var < num_vars; ++var) {
            unit_eqns.emplace_back(Equation::unit(num_vars + 1, var));
        }
    }
    for (std::size_t var = 0; var < num_vars; ++var) {
        const Equation& literal_eqn = unit_eqns[var];
        vsids_params.activity_heap.set_activity(literal_eqn, 0.0);
        vsids_params.activity_heap.insert(literal_eqn);
    }
    vsids_params.inc_amt = 1.0;
}

void VariableVsids::refill_decision_heap() {
    vsids_params.activity_heap.rebuild();
    if (!vsids_params.activity_heap.empty()) {
        return;
    }

    for (std::size_t var = 0; var < num_vars; ++var) {
        const Equation& literal_eqn = unit_eqns[var];
        if (vsids_params.activity_heap.get_priority(literal_eqn) == 0.0) {
            vsids_params.activity_heap.set_activity(literal_eqn, 0.0);
        }
        vsids_params.activity_heap.insert(literal_eqn);
    }
}

Equation VariableVsids::make_decision(const RowSpace& trail_span, const std::vector<Clause>&) {
    while (true) {
        std::optional<Equation> maybe_eqn = vsids_params.activity_heap.extract_max();
        if (!maybe_eqn.has_value()) {
            refill_decision_heap();
            maybe_eqn = vsids_params.activity_heap.extract_max();
            if (!maybe_eqn.has_value()) {
                throw std::runtime_error("VariableVsids::make_decision: activity heap is empty");
            }
        }

        std::size_t leading_col = trail_span.partial_reduce(*maybe_eqn);
        if (leading_col != bit::vector<>::npos && leading_col != num_vars) {
            record_decision(false);
            return *maybe_eqn;
        }
    }
}

void VariableVsids::update_with_critical_equation(const Equation& eqn) {
    std::size_t leading_col = eqn.first_set();
    assert(leading_col < unit_eqns.size());
    vsids_params.bump_activity(unit_eqns[leading_col]);
}

void VariableVsids::update_with_asserting_clause(const Clause& clause) {
    for (std::size_t row_index = 0; row_index < clause.rows(); ++row_index) {
        std::size_t var = clause.row(row_index).first_set();
        assert(var < unit_eqns.size());
        vsids_params.bump_activity(unit_eqns[var]);
    }

    vsids_params.inc_amt *= (1 / vsids_params.decay);
    vsids_params.activity_heap.rebuild();
}

void VariableVsids::update_with_compaction(const search::CompactionTransform& transform,
                                           const RowSpace& trail_span,
                                           const std::vector<Clause>& clauses,
                                           const std::vector<std::uint8_t>& clause_active) {
    (void)trail_span;
    (void)clauses;
    (void)clause_active;
    num_vars = transform.new_num_vars;
    unit_eqns = util::project_basis_keep_order(unit_eqns, transform);
    remap_vsids_heap(vsids_params.activity_heap, transform);
    if (vsids_params.activity_heap.empty()) {
        refill_decision_heap();
    }
}
} // namespace heuristics
