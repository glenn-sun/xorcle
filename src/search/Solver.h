#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "PrintMode.h"
#include "algebra/Equation.h"
#include "algebra/RowSpace.h"
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
    /// @brief Tag for timeout results.
    struct Timeout {};

    /// @brief Tag for unsatisfiable results.
    struct Unsat {};

    /// @brief Tag for satisfiable results with assignment payload.
    struct Sat {
        bit::vector<> values; ///< Assignment values in solver order.

        /// @brief Construct with an assignment vector (moved).
        explicit Sat(bit::vector<> v) : values(std::move(v)) {}
    };

    /// @brief Return true if the result is a timeout.
    bool is_timeout(const std::variant<Timeout, Unsat, Sat>& result);

    /// @brief Return true if the result is unsatisfiable.
    bool is_unsat(const std::variant<Timeout, Unsat, Sat>& result);

    /// @brief Return true if the result is satisfiable.
    bool is_sat(const std::variant<Timeout, Unsat, Sat>& result);

    /// @brief Return the assignment from a SAT result or throw.
    /// @pre result holds ResultCase::Sat.
    const bit::vector<>& unwrap_sat(const std::variant<Timeout, Unsat, Sat>& result);
}

/// @brief Solver result variant.
using Result = std::variant<ResultCase::Timeout, ResultCase::Unsat, ResultCase::Sat>;

/// @brief Main solver class implementing propagation, decision, and learning.
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
    std::vector<std::size_t> active_to_original;

    struct CompactedUnitProofEntry {
        algebra::Equation eqn_original;
        std::optional<heuristics::ClauseId> reason_id;
    };

    struct CompactionStep {
        CompactionTransform transform;
        std::vector<std::size_t> active_to_original_before;
        std::vector<std::size_t> eliminated_original_vars;
        std::vector<algebra::Equation> span_rows_old;
        std::vector<algebra::Equation> lifted_span_rows_original;
    };

    /// @brief Historical compaction steps, oldest to newest.
    std::vector<CompactionStep> compaction_history;

    /// @brief Ordered compacted assumptions used for proof augmentation.
    std::vector<CompactedUnitProofEntry> compacted_units_for_proof;

    /// @brief Next proof clause id to allocate for lines not backed by solver clauses.
    heuristics::ClauseId proof_clause_id_allocator = 0;

    Trail trail; ///< Trail of learned equations (decision levels).
    std::shared_ptr<heuristics::ClauseSelector> clause_sel; ///< Clause selector strategy.
    std::shared_ptr<heuristics::DecisionProcedure> dec_proc; ///< Decision procedure strategy.
    PropagationEngine propagation; ///< Unit propagation engine.
    RestartPolicy restart_policy; ///< Conflict-driven restart scheduler.
    std::shared_ptr<io::ProofLogger> proof_logger; ///< Optional proof logger.

    algebra::Equation working_eqn; ///< Reusable buffer for conflict checks.

    algebra::Equation eqn_false; ///< Constant false equation (1 = 0) in active space.

    /// @brief Lift an active-space equation to original variable indexing.
    algebra::Equation lift_active_equation_to_original(const algebra::Equation& eqn_active) const;

    /// @brief Build a proof-logging clause in original variable indexing.
    algebra::Clause build_proof_clause_from_active(const algebra::Clause& clause_active) const;

    /// @brief Reconstruct a full original-width assignment from active-space assignment.
    bit::vector<> reconstruct_original_assignment(const bit::vector<>& active_assignment) const;

    /// @brief Perform level-0 variable compaction before decisions if trigger conditions are met.
    bool maybe_compact_level0_before_decision();

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
    /// @pre trail ends with a contradiction equation.
    std::pair<algebra::Clause, std::size_t> find_asserting_clause(
        const Trail& trail,
        std::vector<heuristics::ClauseId>* reason_ids = nullptr);

    /// @brief Determine if the set of clauses is satisfiable.
    /// @param stop_flag External stop signal; set to true to stop early.
    Result solve(std::atomic<bool>& stop_flag);

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
