#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "PrintMode.h"
#include "algebra/Equation.h"
#include "algebra/RowSpace.h"
#include "algebra/TrailBasisSpace.h"
#include "bit/vector.h"
#include "heuristics/ClauseSelector.h"
#include "heuristics/DecisionProcedure.h"
#include "search/PropagationEngine.h"
#include "search/RestartPolicy.h"
#include "search/Trail.h"
#include "search/Compaction.h"
#include "io/ProofLogger.h"
#include "util/RandomUtils.h"

namespace search {
namespace ResultCase {
    /// @brief Tag for unsatisfiable results.
    struct Unsat {};

    /// @brief Tag for satisfiable results with assignment payload.
    struct Sat {
        bit::vector<> values; ///< Assignment values in solver order.

        /// @brief Construct with an assignment vector (moved).
        explicit Sat(bit::vector<> v) : values(std::move(v)) {}
    };

    /// @brief Return true if the result is unsatisfiable.
    bool is_unsat(const std::variant<Unsat, Sat>& result);

    /// @brief Return true if the result is satisfiable.
    bool is_sat(const std::variant<Unsat, Sat>& result);

    /// @brief Return the assignment from a SAT result or throw.
    /// @pre result holds ResultCase::Sat.
    const bit::vector<>& unwrap_sat(const std::variant<Unsat, Sat>& result);
}

/// @brief Solver result variant.
using Result = std::variant<ResultCase::Unsat, ResultCase::Sat>;

/// @brief Main solver class implementing propagation, decision, and learning.
/// @note `trail`, `clause_sel`, `dec_proc`, `propagation`, `conflict_check_eqn`, and
/// `false_eqn_active` always live in the current active variable space.
/// `original_num_vars` never changes, `active_var_to_original` maps only the
/// current active space back to original variable indices, and
/// `compaction_history` records the eliminated active spaces needed to recover
/// original assignments and lift proof rows.
class Solver {
private:
    /// @brief Enable variable compaction before decisions.
    bool enable_compaction = true;

    /// @brief Number of conflicts required between successful compactions.
    std::size_t compaction_conflict_interval = 16;

    /// @brief Whether at least one compaction has succeeded.
    bool has_compacted_once = false;

    /// @brief Conflict counter snapshot at the last successful compaction.
    std::size_t conflicts_at_last_compaction = 0;

    /// @brief Number of variables in the original input problem.
    const std::size_t original_num_vars;

    /// @brief Number of variables in the currently active compacted space.
    std::size_t active_num_vars;

    /// @brief Active variable index -> original variable index.
    std::vector<std::size_t> active_var_to_original;

    /// @brief Historical compaction steps, oldest to newest.
    std::vector<CompactionHistoryEntry> compaction_history;

    /// @brief Ordered original-space assumptions carried forward for proof augmentation.
    std::vector<ProofAssumption> proof_assumptions;

    /// @brief Next proof clause id to allocate for lines not backed by solver clauses.
    heuristics::ClauseId next_proof_clause_id = 0;

    Trail trail; ///< Trail of learned equations (decision levels).
    std::shared_ptr<heuristics::ClauseSelector> clause_sel; ///< Clause selector strategy.
    std::shared_ptr<heuristics::DecisionProcedure> dec_proc; ///< Decision procedure strategy.
    PropagationEngine propagation; ///< Unit propagation engine.
    RestartPolicy restart_policy; ///< Conflict-driven restart scheduler.
    std::shared_ptr<io::ProofLogger> proof_logger; ///< Optional proof logger.

    algebra::Equation conflict_check_eqn; ///< Reusable buffer for contradiction checks.

    algebra::Equation false_eqn_active; ///< Constant false equation (1 = 0) in active space.

    /// @brief Replace the live active-space state with a staged compaction result.
    /// @param compaction_plan Fully prepared compaction result in the new active space.
    void apply_compaction(CompactionPlan compaction_plan);

    /// @brief Perform compaction before a decision if trigger conditions are met.
    /// @return True if compaction succeeded and replaced the active solver state.
    /// @note Returning false is normal only when compaction is not currently applicable.
    bool maybe_compact_before_decision();

    /// @brief Propagate until quiescence or conflict.
    /// @return True if propagation learned a contradiction equation.
    bool propagate_until_conflict();

    /// @brief Handle a level-0 conflict and emit the final UNSAT proof lines if enabled.
    Result handle_root_conflict_unsat();

    /// @brief Emit the final UNSAT proof once the root trail already contains a contradiction.
    void write_root_unsat_proof();

    /// @brief Analyze a non-root conflict, optionally restart, and learn the asserting clause.
    void handle_non_root_conflict();

    /// @brief Make the next decision and apply it to trail and clause selector state.
    void make_decision_step();

    /// @brief Resolve one trail-basis step while building an asserting clause.
    /// @param asserting_basis Current asserting clause in trail coordinates.
    /// @param pivot_basis_col First non-zero trail-basis column being eliminated.
    /// @param trail Current conflicting trail.
    /// @param reason_ids Optional output buffer for derivation-order reasons.
    void resolve_asserting_basis_with_reason(algebra::TrailBasisSpace& asserting_basis,
                                            std::size_t pivot_basis_col,
                                            const Trail& trail,
                                            std::vector<heuristics::ClauseId>* reason_ids);

public:
    /// @brief Construct a solver from variable count, clause selector, and decision procedure.
    Solver(std::size_t num_vars,
           std::shared_ptr<heuristics::ClauseSelector> clause_sel_in,
           std::shared_ptr<heuristics::DecisionProcedure> dec_proc,
           std::shared_ptr<io::ProofLogger> proof_logger_in = nullptr,
           const RestartConfig& restart_config = {},
           bool enable_compaction = true,
           std::size_t compaction_conflict_interval = 16);

    /// @brief Construct a solver with a default cyclic clause selector.
    Solver(std::size_t num_vars,
           std::shared_ptr<heuristics::DecisionProcedure> dec_proc,
           std::shared_ptr<io::ProofLogger> proof_logger_in = nullptr,
           const RestartConfig& restart_config = {},
           bool enable_compaction = true,
           std::size_t compaction_conflict_interval = 16);

    /// @brief Return the variable count for this solver instance.
    std::size_t get_num_vars() const;

    /// @brief Return the variable count in the current compacted active space.
    std::size_t get_active_num_vars() const;

    /// @brief Return the number of clauses currently stored.
    std::size_t get_num_clauses() const;

    /// @brief Return current decision statistics from the decision procedure.
    heuristics::DecisionProcedure::DecisionStats decision_stats() const;

    /// @brief Number of decision levels (excluding level 0).
    std::size_t level() const;

    /// @brief Return the current clause list.
    const std::vector<algebra::Clause>& get_clauses() const;

    /// @brief Return a mutable clause reference by id.
    algebra::Clause& get_clause(heuristics::ClauseId id);

    /// @brief Find an asserting clause for the current conflict.
    /// @param trail Current conflicting trail.
    /// @param reason_ids Optional output buffer for clause ids in derivation order.
    /// @return Asserting clause and the number of trailing decision levels it can remove.
    /// @pre `trail` ends with a contradiction equation whose learned row has a reason.
    /// @post The returned clause has width `active_num_vars + 1`.
    /// @post Reversing `reason_ids` once yields proof order for the learned clause.
    std::pair<algebra::Clause, std::size_t> find_asserting_clause(
        const Trail& trail,
        std::vector<heuristics::ClauseId>* reason_ids = nullptr);

    /// @brief Determine if the set of clauses is satisfiable.
    Result solve();

    /// @brief Print all clauses using the given print mode.
    void print_clauses(PrintMode mode = PrintMode{}) const;

    /// @brief Print the current trail using the given equation print mode.
    void print_trail(EquationPrintMode eqn_mode = EquationPrintMode::standard) const;

    /// @brief Create a Solver from a DIMACS file.
    /// @param filename Path to a DIMACS file.
    /// @param dpn Decision procedure to use.
    /// @param csn Clause selector to use.
    /// @param phase Decision phase selection for clause-based decisions.
    /// @param proof_logger Optional proof logger.
    /// @param seed Base RNG seed used to derive deterministic solver sub-streams.
    /// @note The DIMACS format is extended as in Danner/Kreuzer to support XOR literals.
    static Solver from_DIMACS(const std::string& filename,
                              heuristics::DecisionProcedureName dpn,
                              heuristics::ClauseSelectorName csn,
                              heuristics::DecisionPhase phase,
                              const heuristics::CVsidsConfig& cvsids_config = {},
                              std::shared_ptr<io::ProofLogger> proof_logger = nullptr,
                              util::Seed seed = util::default_seed,
                              const RestartConfig& restart_config = {},
                              bool enable_compaction = true,
                              std::size_t compaction_conflict_interval = 16);

    /// @brief Total number of attempted propagations.
    std::size_t num_attempt_props = 0;

    /// @brief Total number of watch updates.
    std::size_t num_watch_updates = 0;

    /// @brief Total number of successful propagations.
    std::size_t num_propagations = 0;

    /// @brief Total number of conflicts encountered.
    std::size_t num_conflicts = 0;

    /// @brief Total number of restarts performed.
    std::size_t num_restarts = 0;
};
} // namespace search
