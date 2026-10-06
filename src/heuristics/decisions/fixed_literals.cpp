#include "heuristics/decisions/fixed_literals.h"

#include <cassert>

#include "heuristics/decisions/clause_decisions.h"

namespace xorcle {

FixedLiterals::FixedLiterals(EquationRepr repr, std::size_t num_vars, Seed seed)
    : DecisionProcedure(repr, num_vars, seed) {
    reinitialize();
}

void FixedLiterals::reinitialize() {
    reset_phase_rng();
    reset_counters();
    decision_basis_.clear();
    decision_basis_.reserve(num_vars());
    for (std::size_t var = 0; var < num_vars(); ++var) {
        decision_basis_.push_back(unit_equation(var));
    }
}

DecisionResult FixedLiterals::make_decision(const RowSpace& trail_span, const std::vector<Clause>&,
                                            const std::vector<std::uint8_t>&,
                                            const SatisfactionRecord&) {
    assert(trail_span.cols() == num_vars() + 1 &&
           "the trail span does not match the procedure's variable count");
    assert(trail_span.repr() == repr() &&
           "the trail span does not match the procedure's representation");
    assert(trail_span.rows() < decision_basis_.size() && "no legal decision remains");

    record_decision();
    return DecisionResult{decide_from_basis(decision_basis_, trail_span), std::nullopt};
}

void FixedLiterals::update_with_substitution(const IndexSet& dropped_col_idxs,
                                             const std::vector<Clause>&,
                                             const std::vector<std::uint8_t>&) {
    assert(dropped_col_idxs.size() == num_vars() + 1 &&
           "the dropped variables are over another numbering");
    std::erase_if(decision_basis_, [&](const Equation& literal) {
        return dropped_col_idxs.test(*literal.leading_bit());
    });
}

void FixedLiterals::update_with_renumbering(const CompactionTransform& transform) {
    assert(transform.old_num_vars() == num_vars() &&
           "the transform was built for a different active variable numbering");
    decision_basis_ = compact_basis_keep_order(decision_basis_, transform, repr());
    set_num_vars(transform.new_num_vars());
}

} // namespace xorcle
