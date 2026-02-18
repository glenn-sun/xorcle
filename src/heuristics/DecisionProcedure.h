#pragma once

#include <cstddef>
#include <deque>
#include <list>
#include <optional>
#include <random>
#include <vector>

#include "MaxHeap.h"
#include "algebra/Equation.h"
#include "algebra/RowSpace.h"
#include "heuristics/Types.h"

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
    cvsids,            ///< Clause-VSIDS with shared state.
    berkmin,           ///< Berkmin-like clause decision strategy.
    cbh               ///< CBH clause ordering heuristic.
};

/// @brief Base class for decision procedures.
class DecisionProcedure {
protected:
    /// @brief Number of variables in the problem.
    std::size_t num_vars;

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

    /// @brief RNG for shuffling and sampling.
    std::mt19937 rng;

public:
    /// @brief Construct with a seeded RNG and initialize state.
    explicit RandomClauseSum(std::size_t n) : DecisionProcedure(n), rng(std::random_device{}()) { reinitialize(); }

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

/// @brief Berkmin-like decision procedure.
class BerkminDecision : public DecisionProcedure {
private:
    /// @brief Queue of recent conflict clauses.
    std::deque<ClauseId> conflict_clauses;

    /// @brief Whether the next learned clause should be enqueued.
    bool pending_conflict_clause = false;

    /// @brief Unit equations for fallback decisions.
    std::vector<Equation> unit_eqns;

    /// @brief Last clause id used to make a decision.
    std::optional<ClauseId> last_decision_clause;

public:
    /// @brief Construct and initialize state.
    explicit BerkminDecision(std::size_t n) : DecisionProcedure(n) { reinitialize(); }

    /// @brief Reset state and counters.
    void reinitialize() override;

    /// @brief Choose a decision based on conflict clauses or unit fallback.
    Equation make_decision(const RowSpace& trail_span, const std::vector<Clause>& clauses) override;

    /// @brief Mark that the next learned clause should be queued.
    void update_with_asserting_clause(const Clause& clause) override;

    /// @brief Enqueue the learned clause if marked.
    void update_with_new_clause(const Clause& clause, ClauseId id) override;

    /// @brief Return the clause id used for the most recent decision.
    std::optional<ClauseId> decision_clause_hint() const override;
};

/// @brief Clause-based heuristic (CBH) decision procedure.
class CBHDecision : public DecisionProcedure {
private:
    /// @brief Ordered list of clause ids used for selection.
    std::list<ClauseId> clause_list;

    /// @brief Positions of clause ids in the list.
    std::vector<std::list<ClauseId>::iterator> positions;

    /// @brief Whether the initial list has been built.
    bool list_built = false;

    /// @brief Iterator to the current list position.
    std::list<ClauseId>::iterator current;

    /// @brief Clause ids responsible for pending reorder.
    std::vector<ClauseId> pending_responsible;

    /// @brief Marks for pending reorder processing.
    std::vector<char> pending_mark;

    /// @brief Whether a conflict clause is pending.
    bool pending_conflict_clause = false;

    /// @brief Clause id associated with a pending conflict.
    std::optional<ClauseId> pending_conflict_clause_id;

    /// @brief Unit equations for fallback decisions.
    std::vector<Equation> unit_eqns;

    /// @brief Last clause id used to make a decision.
    std::optional<ClauseId> last_decision_clause;

    /// @brief Ensure unit equations are initialized.
    void ensure_unit_eqns();

    /// @brief Build the initial clause ordering list.
    void build_initial_list(const std::vector<Clause>& clauses);

    /// @brief Apply a pending reorder after a conflict.
    void apply_pending_reorder(ClauseId conflict_id);

public:
    /// @brief Construct and initialize state.
    explicit CBHDecision(std::size_t n) : DecisionProcedure(n) { reinitialize(); }

    /// @brief Reset state and counters.
    void reinitialize() override;

    /// @brief Choose a decision using the CBH ordering.
    Equation make_decision(const RowSpace& trail_span, const std::vector<Clause>& clauses) override;

    /// @brief Mark that the next learned clause should trigger reorder.
    void update_with_asserting_clause(const Clause& clause) override;

    /// @brief Update internal order when a clause is resolved.
    void update_with_resolved_clause(ClauseId id) override;

    /// @brief Update internal order when a new clause is learned.
    void update_with_new_clause(const Clause& clause, ClauseId id) override;

    /// @brief Return the clause id used for the most recent decision.
    std::optional<ClauseId> decision_clause_hint() const override;
};
} // namespace heuristics
