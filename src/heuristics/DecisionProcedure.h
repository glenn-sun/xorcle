#pragma once

#include <cstddef>
#include <optional>
#include <random>
#include <vector>

#include "MaxHeap.h"
#include "algebra/Equation.h"
#include "algebra/RowSpace.h"
#include "heuristics/Types.h"
#include "util/DecisionUtils.h"

namespace heuristics {
using Clause = algebra::Clause;
using Equation = algebra::Equation;
using RowSpace = algebra::RowSpace;

/// @brief Available decision procedure implementations.
enum class DecisionProcedureName {
    fixed_unit,        ///< Always decide from a fixed unit basis.
    random_clause_sum, ///< Sample decisions from random clauses, fallback to unit basis.
    vsids_unit,        ///< VSIDS on unit equations.
    vsids_clause,      ///< VSIDS on clauses with sampling.
    cvsids             ///< Clause-VSIDS with shared state.
};

/// @brief Tunable parameters for CVsids decision behavior.
struct CVsidsConfig {
    double learned_base = 1.0; ///< Learned-clause initial priority multiplier on current increment.
    double decay = 0.95;       ///< Activity decay factor (0 < decay < 1).
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
    /// @brief Number of variables in the problem.
    std::size_t num_vars;

    /// @brief Phase selection for clause-based decisions.
    DecisionPhase decision_phase = DecisionPhase::unsat;

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
    Equation apply_phase(Equation eqn) const;

public:
    /// @brief Construct a decision procedure for a given variable count.
    explicit DecisionProcedure(std::size_t n) : num_vars(n) {}

    /// @brief Virtual destructor for interface use.
    virtual ~DecisionProcedure() = default;

    /// @brief Reinitialize internal state for a fresh run.
    virtual void reinitialize() = 0;

    /// @brief Choose the next decision equation.
    /// @param trail_span Row space of the current trail.
    /// @param clauses Current set of clauses.
    /// @return A decision equation (not necessarily reduced).
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

    /// @brief Decision statistics snapshot.
    struct DecisionStats {
        /// @brief Number of decisions made.
        std::size_t decisions = 0;

        /// @brief Number of fallback decisions made.
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

/// @brief Fixed unit-basis decision procedure.
class FixedUnit : public DecisionProcedure {
private:
    /// @brief Fixed unit-basis used for decisions.
    std::vector<Equation> decision_basis;

public:
    /// @brief Construct with a fixed unit basis and initialize state.
    explicit FixedUnit(std::size_t n) : DecisionProcedure(n) { reinitialize(); }

    /// @brief Reset unit basis and counters.
    void reinitialize() override;

    /// @brief Choose a decision from the fixed unit basis.
    Equation make_decision(const RowSpace& trail_span, const std::vector<Clause>& clauses) override;
};

/// @brief Random clause-sum decision procedure.
class RandomClauseSum : public DecisionProcedure {
private:
    /// @brief Unit basis used for fallback decisions.
    std::vector<Equation> decision_basis;

    /// @brief Random permutation of clause ids for sampling.
    std::vector<ClauseId> clause_perm;

    /// @brief Reused scratch state for clause satisfaction checks.
    util::ClauseSatisfactionWorkspace clause_sat_ws;

    /// @brief RNG for shuffling and sampling.
    std::mt19937 rng;

public:
    /// @brief Construct with a seeded RNG and initialize state.
    explicit RandomClauseSum(std::size_t n)
        : DecisionProcedure(n), clause_sat_ws(n), rng(std::random_device{}()) { reinitialize(); }

    /// @brief Reset unit basis and counters.
    void reinitialize() override;

    /// @brief Sample a decision from clauses; fallback to unit basis.
    Equation make_decision(const RowSpace& trail_span, const std::vector<Clause>& clauses) override;
};

/// @brief Parameters for VSIDS-based decision procedures.
struct VsidsParams {
    /// @brief Max-heap keyed by equation activity.
    MaxHeap<Equation> activity_heap;

    /// @brief Activity increment amount.
    double inc_amt = 1;

    /// @brief Activity decay factor (0 < decay < 1).
    double decay = 0.95;

    /// @brief Construct with an empty activity heap.
    VsidsParams() : activity_heap() { }

    /// @brief Increase activity for an equation and rescale if needed.
    void bump_activity(const Equation& eqn);
};

/// @brief VSIDS decision procedure on unit equations.
class VsidsUnit : public DecisionProcedure {
protected:
    /// @brief VSIDS parameters and heap.
    VsidsParams vsids_params;

    /// @brief Cached false equation (constant 1).
    Equation eqn_false;

    /// @brief Unit equations for each variable.
    std::vector<Equation> unit_eqns;

public:
    /// @brief Construct and initialize VSIDS state.
    explicit VsidsUnit(std::size_t n)
        : DecisionProcedure(n) { reinitialize(); eqn_false = Equation::unit(num_vars + 1, num_vars); }

    /// @brief Reset VSIDS activity and counters.
    void reinitialize() override;

    /// @brief Select the highest-activity unit decision.
    Equation make_decision(const RowSpace& trail_span, const std::vector<Clause>& clauses) override;

    /// @brief Bump activity for a critical equation.
    void update_with_critical_equation(const Equation& eqn) override;

    /// @brief Update activity after learning an asserting clause.
    void update_with_asserting_clause(const Clause& clause) override;
};

/// @brief VSIDS decision procedure that samples from clauses.
class VsidsClause : public VsidsUnit {
public:
    /// @brief Construct and initialize VSIDS clause procedure.
    explicit VsidsClause(std::size_t n) : VsidsUnit(n) { }

    /// @brief Choose a decision by sampling from clauses.
    Equation make_decision(const RowSpace& trail_span, const std::vector<Clause>& clauses) override;
};
} // namespace heuristics
