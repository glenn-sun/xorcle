#include "heuristics/decisions/fixed_literals.h"

#include "heuristics/decisions/decision_utils.h"

namespace xorcle {

FixedLiterals::FixedLiterals(Algebra backend, std::size_t num_vars, Seed seed)
    : DecisionProcedure(backend, num_vars, seed), reduced_scratch_(backend, num_vars) {
    reinitialize();
}

void FixedLiterals::reinitialize() {
    reset_phase_rng();
    reset_stats();
    decision_basis_.clear();
    decision_basis_.reserve(num_vars());
    for (std::size_t var = 0; var < num_vars(); ++var) {
        decision_basis_.push_back(unit_equation(var));
    }
}

Equation FixedLiterals::make_decision(const RowSpace& trail_span, const std::vector<Clause>&) {
    record_decision(false);
    return decide_from_basis(decision_basis_, trail_span, reduced_scratch_);
}

void FixedLiterals::update_with_compaction(const CompactionTransform& transform, const RowSpace&,
                                           const std::vector<Clause>&,
                                           const std::vector<std::uint8_t>&) {
    decision_basis_ = project_basis_keep_order(decision_basis_, transform, backend());
    set_num_vars(transform.new_num_vars());
    reduced_scratch_.resize(num_vars());
    reduced_scratch_.clear();
}

} // namespace xorcle
