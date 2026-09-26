#include "heuristics/decisions/decision_procedure.h"

#include <cassert>
#include <span>

#include "heuristics/decisions/csids.h"
#include "heuristics/decisions/fixed_literals.h"

namespace xorcle {

DecisionProcedure::DecisionProcedure(EquationRepr repr, std::size_t num_vars, Seed seed)
    : repr_(repr),
      num_vars_(num_vars),
      base_seed_(seed),
      phase_rng_(make_rng(seed, RngComponent::decision_phase)),
      false_equation_(Equation::eq_false(repr, num_vars)) {}

void DecisionProcedure::update_with_asserting_clause(const Clause&) {}

void DecisionProcedure::update_with_new_clause(const Clause&, ClauseId) {}

void DecisionProcedure::update_with_resolved_clause(ClauseId) {}

void DecisionProcedure::update_with_subsumed_clause(ClauseId, ClauseId) {}

// The procedures that walk their whole candidate set on every decision have
// nothing to restore.
void DecisionProcedure::update_with_backjump(std::size_t) {}

std::size_t DecisionProcedure::reusable_levels(const RowSpace&, const std::vector<Clause>&,
                                              const std::vector<std::uint8_t>&,
                                              const std::vector<std::optional<ClauseId>>&,
                                              const SatisfactionRecord&) {
    return 0;
}

std::optional<ClauseId> DecisionProcedure::decision_clause_hint() const {
    return std::nullopt;
}

void DecisionProcedure::update_with_compaction(const CompactionTransform& transform,
                                               const RowSpace&, const std::vector<Clause>&,
                                               const std::vector<std::uint8_t>&) {
    assert(transform.old_num_vars() == num_vars_ &&
           "the transform was built for a different active variable numbering");
    set_num_vars(transform.new_num_vars());
}

void DecisionProcedure::add_to_stats(RunStats& stats) const {
    stats.decisions = counters_.decisions;
    stats.fallback_decisions = counters_.fallback_decisions;
}

void DecisionProcedure::note_filled_decisions(std::size_t num_decisions) {
    counters_.decisions += num_decisions;
    counters_.fallback_decisions += num_decisions;
}

void DecisionProcedure::reset_phase_rng() {
    phase_rng_ = make_rng(base_seed_, RngComponent::decision_phase);
}

void DecisionProcedure::apply_decision_phase(Equation& eqn) const {
    apply_phase(eqn, decision_phase_, false_equation_, phase_rng_);
}

void DecisionProcedure::set_num_vars(std::size_t num_vars) {
    num_vars_ = num_vars;
    false_equation_.resize(num_vars);
    false_equation_.set_eq_false();
}

Equation DecisionProcedure::unit_equation(std::size_t var) const {
    assert(var < num_vars_ && "unit_equation: variable index is out of range");
    return Equation::from_bits(repr_, num_vars_, std::span<const std::size_t>(&var, 1));
}

std::unique_ptr<DecisionProcedure> make_decision_procedure(DecisionProcedureName name,
                                                           EquationRepr repr,
                                                           std::size_t num_vars, Seed seed,
                                                           DecisionPhase phase,
                                                           const CsidsConfig& csids) {
    std::unique_ptr<DecisionProcedure> procedure;
    switch (name) {
        case DecisionProcedureName::fixed_literals:
            procedure = std::make_unique<FixedLiterals>(repr, num_vars, seed);
            break;
        case DecisionProcedureName::csids:
            procedure = std::make_unique<Csids>(repr, num_vars, seed, csids);
            break;
    }
    assert(procedure != nullptr && "make_decision_procedure: unknown decision procedure name");
    procedure->set_decision_phase(phase);
    return procedure;
}

} // namespace xorcle
