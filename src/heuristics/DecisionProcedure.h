#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "MaxHeap.h"
#include "algebra/Equation.h"
#include "algebra/RowSpace.h"
#include "heuristics/Types.h"
#include "search/Compaction.h"
#include "util/DecisionUtils.h"
#include "util/RandomUtils.h"

namespace heuristics {
using Clause = algebra::Clause;
using Equation = algebra::Equation;
using RowSpace = algebra::RowSpace;

/// @brief Available decision procedure implementations.
enum class DecisionProcedureName {
    fixed_literals, ///< Decide from a fixed literal basis.
    variable_vsids, ///< VSIDS over per-variable literal decisions.
    clause_vsids    ///< Clause-VSIDS with shared clause activity state.
};

/// @brief Tunable parameters for Clause VSIDS decision behavior.
struct CVsidsConfig {
    double learned_base = 1.0; ///< Learned-clause initial priority multiplier on current increment.
    double decay = 0.98;       ///< Activity decay factor (0 < decay < 1).
    double bump = 1.0;         ///< Initial activity increment amount.
};

/// @brief Phase selection for clause-based decisions.
enum class DecisionPhase {
    unsat,  ///< Decide equations in the negated clause (current behavior).
    sat,    ///< Decide the negation of the equation in the negated clause.
    random  ///< Randomly choose sat/unsat per decision.
};

/// @brief Base class for decision procedures.
class DecisionProcedure {
protected:
    /// @brief Number of variables in the active problem.
    std::size_t num_vars;

    /// @brief Base seed used to derive all deterministic RNG streams owned by the procedure.
    util::Seed base_seed;

    /// @brief Phase selection for clause-based decisions.
    DecisionPhase decision_phase = DecisionPhase::unsat;

    /// @brief RNG stream used for seeded random phase selection.
    mutable util::Rng phase_rng;

    /// @brief Count of decisions made so far.
    std::size_t decisions = 0;

    /// @brief Count of decisions that fell back to a generic choice.
    std::size_t fallback_decisions = 0;

    /// @brief Record a decision and whether it was a fallback.
    void record_decision(bool used_fallback) {
        ++decisions;
        if (used_fallback) {
            ++fallback_decisions;
        }
    }

    /// @brief Reset decision counters.
    void reset_stats() {
        decisions = 0;
        fallback_decisions = 0;
    }

    /// @brief Apply the configured decision phase to a clause-based decision.
    /// @param eqn Clause-derived decision equation in unsat-phase form.
    /// @return Decision equation with the configured phase applied.
    Equation apply_phase(Equation eqn) const;

    /// @brief Reset the phase RNG for a fresh run.
    void reset_phase_rng();

public:
    /// @brief Construct a decision procedure for a given active variable count.
    explicit DecisionProcedure(std::size_t n, util::Seed seed)
        : num_vars(n),
          base_seed(seed),
          phase_rng(util::make_rng(seed, util::RngComponent::decision_phase)) {}

    /// @brief Virtual destructor for interface use.
    virtual ~DecisionProcedure() = default;

    /// @brief Reinitialize procedure-owned state for a fresh solver run.
    /// @note This is a fresh-run reset, not a mid-solve refill path.
    virtual void reinitialize() = 0;

    /// @brief Choose the next decision equation.
    /// @param trail_span Current trail span.
    /// @param clauses Current clause database in clause-id order.
    /// @return A decision equation in the current active width.
    /// @pre The solver calls this only when at least one legal decision remains.
    /// @post The returned equation has width `num_vars + 1`.
    virtual Equation make_decision(const RowSpace& trail_span, const std::vector<Clause>& clauses) = 0;

    /// @brief Update procedure based on a critical equation during analysis.
    virtual void update_with_critical_equation(const Equation& eqn) { (void)eqn; }

    /// @brief Update procedure after learning an asserting clause.
    virtual void update_with_asserting_clause(const Clause& clause) { (void)clause; }

    /// @brief Notify procedure of a newly added clause.
    virtual void update_with_new_clause(const Clause& clause, ClauseId id) {
        (void)clause;
        (void)id;
    }

    /// @brief Notify procedure that a clause was resolved during analysis.
    virtual void update_with_resolved_clause(ClauseId id) { (void)id; }

    /// @brief Optional hint for which clause to prioritize next.
    virtual std::optional<ClauseId> decision_clause_hint() const { return std::nullopt; }

    /// @brief Notify procedure after clauses/variable space were compacted.
    /// @param transform Compaction transform from old active space to new active space.
    /// @param trail_span Current compacted trail span.
    /// @param clauses Current compacted clauses.
    /// @param clause_active Per-clause active flags in clause-id order.
    /// @note This remaps decision state into the new active width; it is not a fresh-run reset.
    virtual void update_with_compaction(const search::CompactionTransform& transform,
                                        const RowSpace& trail_span,
                                        const std::vector<Clause>& clauses,
                                        const std::vector<std::uint8_t>& clause_active) {
        (void)trail_span;
        (void)clauses;
        (void)clause_active;
        num_vars = transform.new_num_vars;
    }

    /// @brief Decision statistics snapshot.
    struct DecisionStats {
        std::size_t decisions = 0;
        std::size_t fallback_decisions = 0;
    };

    /// @brief Return current decision statistics.
    virtual DecisionStats decision_stats() const {
        return DecisionStats{decisions, fallback_decisions};
    }

    /// @brief Set the phase selection for clause-based decisions.
    void set_decision_phase(DecisionPhase phase) { decision_phase = phase; }

    /// @brief Get the phase selection for clause-based decisions.
    DecisionPhase get_decision_phase() const { return decision_phase; }
};

/// @brief Decision procedure that scans a fixed literal basis.
class FixedLiterals : public DecisionProcedure {
private:
    /// @brief Fixed literal basis in variable order.
    std::vector<Equation> decision_basis;

public:
    explicit FixedLiterals(std::size_t n, util::Seed seed) : DecisionProcedure(n, seed) { reinitialize(); }

    /// @brief Reset the fixed literal basis for a fresh run.
    void reinitialize() override;

    /// @brief Choose the first non-implied literal basis equation.
    Equation make_decision(const RowSpace& trail_span, const std::vector<Clause>& clauses) override;

    void update_with_compaction(const search::CompactionTransform& transform,
                                const RowSpace& trail_span,
                                const std::vector<Clause>& clauses,
                                const std::vector<std::uint8_t>& clause_active) override;
};

/// @brief Parameters for Variable VSIDS decision procedures.
struct VsidsParams {
    MaxHeap<Equation> activity_heap; ///< Heap keyed by equation activity.
    double inc_amt = 1.0;            ///< Activity increment amount.
    double decay = 0.98;             ///< Activity decay factor (0 < decay < 1).

    /// @brief Increase activity for an equation and rescale if needed.
    void bump_activity(const Equation& eqn);
};

/// @brief Variable VSIDS decision procedure over per-variable literal equations.
class VariableVsids : public DecisionProcedure {
protected:
    /// @brief VSIDS parameters and heap.
    VsidsParams vsids_params;

    /// @brief Unit equations for each active variable.
    std::vector<Equation> unit_eqns;

    /// @brief Reset the heap for a fresh run.
    void reset_decision_heap_for_fresh_run();

    /// @brief Refill an unexpectedly empty heap during solving.
    void refill_decision_heap();

public:
    explicit VariableVsids(std::size_t n, util::Seed seed)
        : DecisionProcedure(n, seed) {
        reinitialize();
    }

    /// @brief Reset VSIDS activity and counters for a fresh run.
    void reinitialize() override;

    /// @brief Select the highest-activity legal literal decision.
    Equation make_decision(const RowSpace& trail_span, const std::vector<Clause>& clauses) override;

    /// @brief Bump activity for a critical equation.
    void update_with_critical_equation(const Equation& eqn) override;

    /// @brief Update activity after learning an asserting clause.
    void update_with_asserting_clause(const Clause& clause) override;

    void update_with_compaction(const search::CompactionTransform& transform,
                                const RowSpace& trail_span,
                                const std::vector<Clause>& clauses,
                                const std::vector<std::uint8_t>& clause_active) override;
};
} // namespace heuristics
