/// @file
/// @brief Solver orchestration and conflict analysis. This file is the main place
/// to read how the solver progresses through propagation, conflict handling,
/// compaction, and decisions.

#include "search/Solver.h"

#include <algorithm>
#include <cassert>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <utility>

#include "heuristics/Factory.h"
#include "io/DimacsParser.h"
#include "search/OriginalSpace.h"

namespace search {

bool ResultCase::is_timeout(const std::variant<Timeout, Unsat, Sat>& result) {
    return std::holds_alternative<Timeout>(result);
}

bool ResultCase::is_unsat(const std::variant<Timeout, Unsat, Sat>& result) {
    return std::holds_alternative<Unsat>(result);
}

bool ResultCase::is_sat(const std::variant<Timeout, Unsat, Sat>& result) {
    return std::holds_alternative<Sat>(result);
}

const bit::vector<>& ResultCase::unwrap_sat(const std::variant<Timeout, Unsat, Sat>& result) {
    if (const Sat* sat = std::get_if<Sat>(&result)) {
        return sat->values;
    }
    throw std::runtime_error("Expected Sat result");
}

Solver::Solver(std::size_t num_vars,
               std::shared_ptr<heuristics::ClauseSelector> clause_selector,
               std::shared_ptr<heuristics::DecisionProcedure> decision_procedure,
               std::shared_ptr<io::ProofLogger> proof_logger_in,
               const RestartConfig& restart_config,
               bool enable_compaction_in,
               std::size_t compaction_conflict_interval_in)
    : enable_compaction(enable_compaction_in),
      compaction_conflict_interval(compaction_conflict_interval_in),
      original_num_vars(num_vars),
      active_num_vars(num_vars),
      active_var_to_original(num_vars),
      trail(num_vars),
      clause_sel(std::move(clause_selector)),
      dec_proc(std::move(decision_procedure)),
      propagation(num_vars),
      restart_policy(restart_config),
      proof_logger(std::move(proof_logger_in)),
      conflict_check_eqn(bit::vector<>::zeros(num_vars + 1)),
      false_eqn_active(algebra::Equation::unit(num_vars + 1, num_vars)) {
    std::iota(active_var_to_original.begin(), active_var_to_original.end(), 0);
}

Solver::Solver(std::size_t num_vars,
               std::shared_ptr<heuristics::DecisionProcedure> decision_procedure,
               std::shared_ptr<io::ProofLogger> proof_logger_in,
               const RestartConfig& restart_config,
               bool enable_compaction_in,
               std::size_t compaction_conflict_interval_in)
    : Solver(num_vars,
             std::make_shared<heuristics::Cyclic>(num_vars),
             std::move(decision_procedure),
             std::move(proof_logger_in),
             restart_config,
             enable_compaction_in,
             compaction_conflict_interval_in) {}

std::size_t Solver::get_num_vars() const { return original_num_vars; }

std::size_t Solver::get_active_num_vars() const { return active_num_vars; }

std::size_t Solver::get_num_clauses() const { return clause_sel->get_num_clauses(); }

heuristics::DecisionProcedure::DecisionStats Solver::decision_stats() const {
    return dec_proc->decision_stats();
}

std::size_t Solver::level() const { return trail.size() - 1; }

const std::vector<algebra::Clause>& Solver::get_clauses() const {
    return clause_sel->get_clauses();
}

algebra::Clause& Solver::get_clause(heuristics::ClauseId id) {
    return clause_sel->get_clause(id);
}

std::vector<ProofAssumption> Solver::collect_proof_assumptions_from_trail(
    const CompactionHistoryEntry& history_entry) const {
    std::vector<ProofAssumption> new_assumptions;
    new_assumptions.reserve(trail.reasons().size());

    auto already_recorded = [&](const algebra::Equation& original_eqn) {
        for (const ProofAssumption& existing_assumption : proof_assumptions) {
            if (existing_assumption.original_eqn == original_eqn) {
                return true;
            }
        }
        for (const ProofAssumption& new_assumption : new_assumptions) {
            if (new_assumption.original_eqn == original_eqn) {
                return true;
            }
        }
        return false;
    };

    for (std::size_t learned_index = 0; learned_index < trail.reasons().size(); ++learned_index) {
        const std::size_t trail_row_index = trail.order_map()[learned_index];
        const algebra::Equation& active_eqn = trail.span().row(trail_row_index);
        algebra::Equation original_eqn =
            lift_with_variable_map(active_eqn, history_entry.active_var_to_original_before, original_num_vars);
        if (already_recorded(original_eqn)) {
            continue;
        }
        new_assumptions.push_back(ProofAssumption{original_eqn, trail.reasons()[learned_index]});
    }

    return new_assumptions;
}

void Solver::apply_compaction(CompactionPlan compaction_plan) {
    const std::size_t num_clauses = clause_sel->get_num_clauses();
    if (compaction_plan.rewritten_clauses.size() != num_clauses ||
        compaction_plan.clause_is_active.size() != num_clauses) {
        throw std::logic_error("Solver::apply_compaction: staged clause data is desynchronized");
    }

    for (heuristics::ClauseId clause_id = 0; clause_id < num_clauses; ++clause_id) {
        get_clause(clause_id) = std::move(compaction_plan.rewritten_clauses[clause_id]);
        clause_sel->set_clause_active(clause_id, compaction_plan.clause_is_active[clause_id] != 0);
    }

    std::vector<std::size_t> compacted_active_var_to_original(compaction_plan.transform.new_num_vars);
    for (std::size_t active_var = 0; active_var < compaction_plan.transform.new_num_vars; ++active_var) {
        compacted_active_var_to_original[active_var] =
            active_var_to_original[compaction_plan.transform.new_to_old[active_var]];
    }

    active_num_vars = compaction_plan.transform.new_num_vars;
    active_var_to_original = std::move(compacted_active_var_to_original);
    compaction_history.push_back(std::move(compaction_plan.history_entry));
    proof_assumptions.insert(proof_assumptions.end(),
                             std::make_move_iterator(compaction_plan.proof_assumptions.begin()),
                             std::make_move_iterator(compaction_plan.proof_assumptions.end()));

    propagation.reconfigure(active_num_vars);
    false_eqn_active = algebra::Equation::unit(active_num_vars + 1, active_num_vars);
    conflict_check_eqn.resize(active_num_vars + 1);
    conflict_check_eqn.reset();
    trail = Trail(active_num_vars);
    clause_sel->update_with_compaction(compaction_plan.transform, trail);
    dec_proc->update_with_compaction(
        compaction_plan.transform, trail.span(), clause_sel->get_clauses(), clause_sel->active_flags());
    has_compacted_once = true;
    conflicts_at_last_compaction = num_conflicts;
}

bool Solver::maybe_compact_before_decision() {
    const CompactionInputs compaction_inputs{
        .trail = trail,
        .clauses = clause_sel->get_clauses(),
        .clause_is_active = clause_sel->active_flags(),
        .active_var_to_original = active_var_to_original,
        .existing_proof_assumptions = proof_assumptions,
        .active_num_vars = active_num_vars,
        .original_num_vars = original_num_vars,
        .has_compacted_once = has_compacted_once,
        .num_conflicts = num_conflicts,
        .conflicts_at_last_compaction = conflicts_at_last_compaction,
        .compaction_conflict_interval = compaction_conflict_interval,
    };
    if (!search::should_compact_before_decision(compaction_inputs)) {
        return false;
    }

    const CompactionTransform compaction_transform =
        search::build_compaction_transform(trail, active_num_vars);
    if (compaction_transform.new_num_vars == active_num_vars) {
        return false;
    }

    CompactionPlan compaction_plan =
        search::build_compaction_plan(compaction_inputs, compaction_transform);
    apply_compaction(std::move(compaction_plan));
    return true;
}

void Solver::resolve_asserting_basis_with_reason(algebra::TrailBasisSpace& asserting_clause_basis,
                                                 std::size_t pivot_column,
                                                 const Trail& trail,
                                                 std::vector<heuristics::ClauseId>* reason_ids) {
    algebra::Coefficients asserting_row = asserting_clause_basis.pop_first_row();

    // Trail-basis column k corresponds to one learned trail row in reverse order.
    // Resolving against that row's reason clause removes the selected trail coordinate.
    // In trail coordinates, the reason row loses the explicit +1 because the negated
    // learned equation is already one of the trail basis vectors.
    const std::size_t reason_index = asserting_clause_basis.cols() - 1 - pivot_column;
    if (reason_index >= trail.reasons().size()) {
        throw std::logic_error("Solver::resolve_asserting_basis_with_reason: trail reason index is out of range");
    }

    const std::optional<heuristics::ClauseId> reason_clause_id = trail.reasons()[reason_index];
    if (!reason_clause_id.has_value()) {
        throw std::logic_error("Solver::resolve_asserting_basis_with_reason: resolution step requires a missing reason");
    }
    if (*reason_clause_id >= clause_sel->get_num_clauses()) {
        throw std::logic_error("Solver::resolve_asserting_basis_with_reason: resolution reason id is out of range");
    }
    if (reason_ids) {
        reason_ids->push_back(*reason_clause_id);
    }

    algebra::RowSpace reason_clause = get_clause(*reason_clause_id);
    dec_proc->update_with_resolved_clause(*reason_clause_id);
    algebra::TrailBasisSpace reason_basis = algebra::TrailBasisSpace::from_RowSpace(reason_clause, trail);
    algebra::Coefficients reason_row = reason_basis.pop_first_row();

    asserting_clause_basis.add_row(asserting_row + reason_row + algebra::Coefficients::unit(asserting_row.size(), 0));
    for (algebra::Coefficients extra_reason_row : reason_basis) {
        asserting_clause_basis.add_row(extra_reason_row);
    }

#ifndef NDEBUG
    assert(reason_index < trail.order_map().size());
    assert(trail.order_map()[reason_index] < trail.span().rows());
#endif
    dec_proc->update_with_critical_equation(trail.span().row(trail.order_map()[reason_index]));
}

std::pair<algebra::Clause, std::size_t> Solver::find_asserting_clause(
    const Trail& trail,
    std::vector<heuristics::ClauseId>* reason_ids) {
    if (trail.span().rows() == 0) {
        throw std::logic_error("Solver::find_asserting_clause: conflicting trail cannot be empty");
    }

    const std::optional<heuristics::ClauseId> last_reason_clause_id =
        trail.reasons()[trail.span().rows() - 1];
    if (!last_reason_clause_id.has_value()) {
        throw std::logic_error("Solver::find_asserting_clause: conflicting trail row is missing a reason");
    }
    if (*last_reason_clause_id >= clause_sel->get_num_clauses()) {
        throw std::logic_error("Solver::find_asserting_clause: conflicting reason id is out of range");
    }
    if (reason_ids) {
        reason_ids->push_back(*last_reason_clause_id);
    }

    algebra::Clause& last_reason_clause = get_clause(*last_reason_clause_id);
    algebra::TrailBasisSpace asserting_clause_basis =
        algebra::TrailBasisSpace::from_RowSpace(last_reason_clause, trail);

    while (asserting_clause_basis.jumpable_levels() == 0) {
        std::size_t pivot_column = 0;
        while (pivot_column < asserting_clause_basis.cols() &&
               asserting_clause_basis.col(pivot_column).none()) {
            ++pivot_column;
        }
        if (pivot_column == asserting_clause_basis.cols()) {
            throw std::logic_error(
                "Solver::find_asserting_clause: reached zero jumpable levels without a resolvable column");
        }

        resolve_asserting_basis_with_reason(asserting_clause_basis, pivot_column, trail, reason_ids);

#ifndef NDEBUG
        for (std::size_t cleared_column = 0; cleared_column <= pivot_column; ++cleared_column) {
            assert(asserting_clause_basis.col(cleared_column).none());
        }
#endif
    }

    algebra::Clause asserting_clause = asserting_clause_basis.to_RowSpace();
    const std::size_t jumpable_levels = asserting_clause_basis.jumpable_levels();
#ifndef NDEBUG
    assert(asserting_clause.cols() == active_num_vars + 1);
#endif
    if (jumpable_levels >= trail.size()) {
        throw std::logic_error("Solver::find_asserting_clause: jumpable level count exceeds trail size");
    }
    return std::make_pair(std::move(asserting_clause), jumpable_levels);
}

bool Solver::propagate_until_conflict() {
#ifndef NDEBUG
    assert(active_var_to_original.size() == active_num_vars);
    assert(false_eqn_active.size() == active_num_vars + 1);
    assert(conflict_check_eqn.size() == active_num_vars + 1);
#endif

    while (true) {
        auto propagation_result =
            propagation.unit_propagate_once(*clause_sel, trail, num_attempt_props, num_watch_updates);
        if (!propagation_result.has_value()) {
            return false;
        }

        ++num_propagations;
        auto [propagated_eqn, reason_clause_id] = *propagation_result;
#ifndef NDEBUG
        assert(propagated_eqn.size() == active_num_vars + 1);
#endif
        (void)trail.span().partial_reduce(propagated_eqn);
        trail.learn(propagated_eqn, reason_clause_id);
        clause_sel->update_with_propagation(propagated_eqn, trail);

        conflict_check_eqn.unsafe_copy(false_eqn_active);
        if (trail.span().partial_reduce(conflict_check_eqn) == bit::vector<>::npos) {
            return true;
        }
    }
}

Result Solver::handle_root_conflict_unsat() {
    write_root_unsat_proof();
    return ResultCase::Unsat {};
}

void Solver::write_root_unsat_proof() {
    if (!proof_logger) {
        return;
    }

    next_proof_clause_id = std::max(next_proof_clause_id, clause_sel->get_num_clauses());
    RootUnsatProofPlan proof_plan =
        build_root_unsat_proof_plan(trail.reasons(), proof_assumptions, original_num_vars);

    if (proof_plan.guard_clause.has_value()) {
        const heuristics::ClauseId guard_clause_id = next_proof_clause_id++;
        proof_logger->log_learned_clause(guard_clause_id,
                                         *proof_plan.guard_clause,
                                         proof_plan.conflict_reason_ids);
        proof_plan.final_reason_ids.push_back(guard_clause_id);
        const heuristics::ClauseId empty_clause_id = next_proof_clause_id++;
        proof_logger->log_empty_clause(empty_clause_id, proof_plan.final_reason_ids);
        return;
    }

    const heuristics::ClauseId final_clause_id = next_proof_clause_id++;
    proof_logger->log_empty_clause(final_clause_id, proof_plan.conflict_reason_ids);
}

void Solver::handle_non_root_conflict() {
    std::vector<heuristics::ClauseId> reason_ids;
    auto [asserting_clause, jumpable_levels] =
        find_asserting_clause(trail, proof_logger ? &reason_ids : nullptr);
    const heuristics::ClauseId new_clause_id = clause_sel->get_num_clauses();

    dec_proc->update_with_asserting_clause(asserting_clause);

    assert((asserting_clause.rows() != 1) || (jumpable_levels == trail.size() - 1));
    const bool restart_now = restart_policy.should_restart_after_conflict();
    std::size_t removed_levels = jumpable_levels;
    if (restart_now) {
        removed_levels = trail.size() - 1;
        trail.erase(trail.begin() + 1, trail.end());
        restart_policy.note_restart();
        ++num_restarts;
    } else {
        trail.erase(trail.end() - jumpable_levels, trail.end());
    }

    dec_proc->update_with_new_clause(asserting_clause, new_clause_id);
    if (proof_logger) {
        std::reverse(reason_ids.begin(), reason_ids.end());
        const bool needs_original_space_clause =
            (active_num_vars != original_num_vars) || !proof_assumptions.empty();
        if (needs_original_space_clause) {
            algebra::Clause proof_clause = search::build_proof_clause_in_original_space(
                asserting_clause, proof_assumptions, active_var_to_original, original_num_vars);
            proof_logger->log_learned_clause(new_clause_id, proof_clause, reason_ids);
        } else {
            proof_logger->log_learned_clause(new_clause_id, asserting_clause, reason_ids);
        }
    }

    clause_sel->add_asserting_clause(std::move(asserting_clause), trail, removed_levels);
    next_proof_clause_id = std::max(next_proof_clause_id, clause_sel->get_num_clauses());
}

void Solver::make_decision_step() {
    algebra::Equation decision_eqn = dec_proc->make_decision(trail.span(), clause_sel->get_clauses());
    [[maybe_unused]] const std::size_t leading_col = trail.span().partial_reduce(decision_eqn);
#ifndef NDEBUG
    assert(decision_eqn.size() == active_num_vars + 1);
    assert(leading_col != bit::vector<>::npos);
    assert(leading_col != active_num_vars);
#endif
    trail.learn(decision_eqn, std::nullopt);
    clause_sel->update_with_decision(decision_eqn, trail);
    clause_sel->update_with_decision_clause(dec_proc->decision_clause_hint());
}

Result Solver::solve(std::atomic<bool>& stop_flag) {
    next_proof_clause_id = std::max(next_proof_clause_id, clause_sel->get_num_clauses());

    while (stop_flag.load() == false) {
        const bool found_conflict = propagate_until_conflict();
        if (found_conflict) {
            ++num_conflicts;
            if (level() == 0) {
                return handle_root_conflict_unsat();
            }
            handle_non_root_conflict();
            continue;
        }

        if (trail.span().rows() == active_num_vars) {
            bit::vector<> active_assignment = trail.span().get_unique_solution();
            bit::vector<> original_assignment = search::reconstruct_original_assignment(
                active_assignment, compaction_history, active_var_to_original, original_num_vars);
            return ResultCase::Sat {std::move(original_assignment)};
        }

        if (enable_compaction && maybe_compact_before_decision()) {
            continue;
        }

        make_decision_step();
    }
    return ResultCase::Timeout {};
}

void Solver::print_clauses(const PrintMode mode) const {
    std::cout << "Solver with " << original_num_vars << " variables, " << get_num_clauses() << " clauses:" << std::endl;
    const std::vector<algebra::Clause>& clauses = get_clauses();
    for (const auto& clause : clauses) {
        std::cout << clause.to_string(mode) << std::endl;
    }
}

void Solver::print_trail(const EquationPrintMode eqn_mode) const {
    PrintMode mode{eqn_mode, ClausePrintMode::as_rowspace};

    std::cout << "Trail of length " << trail.span().rows() << ":" << std::endl;
    algebra::Clause trail_as_clause = trail.span();
    std::cout << "Final row space " << std::endl << trail_as_clause.to_string(mode) << std::endl;

    for (std::size_t learned_index = 0; learned_index < trail.span().rows(); ++learned_index) {
        algebra::Equation learned_eqn = trail.span().row(trail.order_map()[learned_index]);
        std::cout << "Learning " << learned_eqn.to_string(mode) << std::endl;
        if (trail.reasons()[learned_index]) {
            std::cout << "Reason: " << std::endl
                      << get_clauses()[*trail.reasons()[learned_index]].to_string(mode) << std::endl;
        } else {
            std::cout << "Reason: decision" << std::endl;
        }
    }
}

Solver Solver::from_DIMACS(const std::string& filename,
                           const heuristics::DecisionProcedureName dpn,
                           const heuristics::ClauseSelectorName csn,
                           const heuristics::DecisionPhase phase,
                           const heuristics::CVsidsConfig& cvsids_config,
                           std::shared_ptr<io::ProofLogger> proof_logger,
                           util::Seed seed,
                           const RestartConfig& restart_config,
                           bool enable_compaction,
                           std::size_t compaction_conflict_interval) {
    io::ParsedDimacs parsed = io::parse_dimacs_file(filename);

    std::shared_ptr<heuristics::DecisionProcedure> decision_procedure =
        heuristics::create_decision_procedure(dpn, parsed.num_vars, phase, seed, cvsids_config);
    std::shared_ptr<heuristics::ClauseSelector> clause_selector =
        heuristics::create_clause_selector(csn, parsed.num_vars);

    Solver solver{parsed.num_vars,
                  std::move(clause_selector),
                  std::move(decision_procedure),
                  std::move(proof_logger),
                  restart_config,
                  enable_compaction,
                  compaction_conflict_interval};

    for (auto& clause : parsed.clauses) {
        const heuristics::ClauseId new_clause_id = solver.clause_sel->get_num_clauses();
        solver.dec_proc->update_with_new_clause(clause, new_clause_id);
        solver.clause_sel->add_starting_clause(std::move(clause));
    }

    solver.next_proof_clause_id = solver.clause_sel->get_num_clauses();
    return solver;
}

} // namespace search
