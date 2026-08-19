#include "heuristics/decisions/variable_vsids.h"

#include <cassert>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "algebra/reduce_result.h"
#include "heuristics/decisions/clause_sampling.h"

namespace xorcle {

namespace {

/// @brief Rewrite every key of an activity heap through a compaction.
///
/// Compaction is not injective: two literal equations can land on the same
/// compacted equation, and their activities are then summed. A key compacting to
/// @f$\mathbf{0}@f$ or @f$\mathbf{1}@f$ is dropped, neither being a legal
/// decision. The heap and popped lists are filtered against what survives.
void remap_vsids_heap(MaxHeap<Equation, EquationHash>& heap,
                      const CompactionTransform& transform) {
    using Snapshot = MaxHeap<Equation, EquationHash>::StateSnapshot;
    Snapshot old_snapshot = heap.snapshot();
    Snapshot next_snapshot;

    std::unordered_map<Equation, double, EquationHash> activity_by_compacted;
    activity_by_compacted.reserve(old_snapshot.activity.size());
    for (const auto& [equation, score] : old_snapshot.activity) {
        Equation compacted = equation.compact(transform);
        if (!compacted.classify().is_leading()) {
            continue;
        }
        activity_by_compacted[std::move(compacted)] += score;
    }
    next_snapshot.activity.reserve(activity_by_compacted.size());
    for (const auto& [equation, score] : activity_by_compacted) {
        next_snapshot.activity.emplace(equation, score);
    }

    const auto compact_list = [&](const std::vector<Equation>& source,
                                  std::vector<Equation>& destination) {
        std::unordered_set<Equation, EquationHash> seen;
        seen.reserve(source.size());
        for (const Equation& equation : source) {
            Equation compacted = equation.compact(transform);
            if (next_snapshot.activity.find(compacted) == next_snapshot.activity.end()) {
                continue;
            }
            if (seen.insert(compacted).second) {
                destination.push_back(std::move(compacted));
            }
        }
    };
    compact_list(old_snapshot.heap, next_snapshot.heap);
    compact_list(old_snapshot.popped, next_snapshot.popped);

    heap.load_snapshot(std::move(next_snapshot));
}

} // namespace

void VsidsParams::bump_activity(const Equation& eqn) {
    activity_heap.increase_key(eqn, inc_amt);
    if (activity_heap.get_priority(eqn) > 1e100) {
        activity_heap.rescale(1e-100);
        inc_amt *= 1e-100;
    }
}

VariableVsids::VariableVsids(EquationRepr repr, std::size_t num_vars, Seed seed)
    : DecisionProcedure(repr, num_vars, seed) {
    reinitialize();
}

void VariableVsids::reinitialize() {
    reset_phase_rng();
    reset_stats();
    reset_decision_heap_for_fresh_run();
}

void VariableVsids::reset_decision_heap_for_fresh_run() {
    vsids_params_.activity_heap.reset();
    if (unit_eqns_.size() != num_vars()) {
        unit_eqns_.clear();
        unit_eqns_.reserve(num_vars());
        for (std::size_t var = 0; var < num_vars(); ++var) {
            unit_eqns_.push_back(unit_equation(var));
        }
    }
    for (std::size_t var = 0; var < num_vars(); ++var) {
        vsids_params_.activity_heap.set_activity(unit_eqns_[var], 0.0);
        vsids_params_.activity_heap.insert(unit_eqns_[var]);
    }
    vsids_params_.inc_amt = 1.0;
}

void VariableVsids::refill_decision_heap() {
    vsids_params_.activity_heap.rebuild();
    if (!vsids_params_.activity_heap.empty()) {
        return;
    }
    for (std::size_t var = 0; var < num_vars(); ++var) {
        const Equation& literal = unit_eqns_[var];
        if (vsids_params_.activity_heap.get_priority(literal) == 0.0) {
            vsids_params_.activity_heap.set_activity(literal, 0.0);
        }
        vsids_params_.activity_heap.insert(literal);
    }
}

Equation VariableVsids::make_decision(const RowSpace& trail_span, const std::vector<Clause>&) {
    while (true) {
        std::optional<Equation> candidate = vsids_params_.activity_heap.extract_max();
        if (!candidate.has_value()) {
            refill_decision_heap();
            candidate = vsids_params_.activity_heap.extract_max();
            assert(candidate.has_value() && "VariableVsids: the activity heap is empty");
        }
        if (trail_span.partial_reduce(*candidate).is_leading()) {
            record_decision(false);
            return std::move(*candidate);
        }
    }
}

void VariableVsids::update_with_critical_equation(const Equation& eqn) {
    const ReduceResult leading = eqn.classify();
    assert(leading.is_leading() && leading.bit < unit_eqns_.size() &&
           "critical equation has no variable leading bit");
    vsids_params_.bump_activity(unit_eqns_[leading.bit]);
}

void VariableVsids::update_with_asserting_clause(const Clause& clause) {
    for (std::size_t row_index = 0; row_index < clause.rows(); ++row_index) {
        const ReduceResult leading = clause.row(row_index).classify();
        assert(leading.is_leading() && leading.bit < unit_eqns_.size() &&
               "asserting clause row has no variable leading bit");
        vsids_params_.bump_activity(unit_eqns_[leading.bit]);
    }
    vsids_params_.inc_amt *= (1.0 / vsids_params_.decay);
    vsids_params_.activity_heap.rebuild();
}

void VariableVsids::update_with_compaction(const CompactionTransform& transform, const RowSpace&,
                                           const std::vector<Clause>&,
                                           const std::vector<std::uint8_t>&) {
    unit_eqns_ = compact_basis_keep_order(unit_eqns_, transform, repr());
    set_num_vars(transform.new_num_vars());
    remap_vsids_heap(vsids_params_.activity_heap, transform);
    if (vsids_params_.activity_heap.empty()) {
        refill_decision_heap();
    }
}

} // namespace xorcle
