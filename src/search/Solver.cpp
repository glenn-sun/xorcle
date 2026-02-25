#include <algorithm>
#include <cassert>
#include <iostream>
#include <stdexcept>

#include "search/Solver.h"

#include "algebra/TrailBasisSpace.h"
#include "heuristics/ClauseBasedDecisions.h"
#include "heuristics/Factory.h"
#include "io/DimacsParser.h"

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
               std::shared_ptr<heuristics::ClauseSelector> clause_sel_in,
               std::shared_ptr<heuristics::DecisionProcedure> dec_proc,
               std::shared_ptr<io::ProofLogger> proof_logger_in,
               const RestartConfig& restart_config)
    : num_vars(num_vars),
      trail(num_vars),
      clause_sel(std::move(clause_sel_in)),
      dec_proc(std::move(dec_proc)),
      propagation(num_vars),
      restart_policy(restart_config),
      proof_logger(std::move(proof_logger_in)),
      working_eqn(bit::vector<>::zeros(num_vars + 1)) {}

Solver::Solver(std::size_t num_vars,
               std::shared_ptr<heuristics::DecisionProcedure> dec_proc,
               std::shared_ptr<io::ProofLogger> proof_logger_in,
               const RestartConfig& restart_config)
    : Solver(num_vars, std::make_shared<heuristics::Cyclic>(num_vars), std::move(dec_proc),
             std::move(proof_logger_in), restart_config) {}

std::size_t Solver::get_num_vars() const { return num_vars; }

std::size_t Solver::get_num_clauses() const {
    return clause_sel->get_num_clauses();
}

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

std::pair<algebra::Clause, std::size_t> Solver::find_asserting_clause(
    const Trail& trail,
    std::vector<heuristics::ClauseId>* reason_ids) {
    // Fact: last_reason should never be std::nullopt here
    std::optional<heuristics::ClauseId> last_reason_id = trail.reasons()[trail.span().rows() - 1];
    if (reason_ids && last_reason_id.has_value()) {
        reason_ids->push_back(last_reason_id.value());
    }
    algebra::Clause& last_reason = get_clause(last_reason_id.value());
    algebra::TrailBasisSpace as_tbs = algebra::TrailBasisSpace::from_RowSpace(last_reason, trail);

    std::size_t non_zero_col = 0;
    while (as_tbs.jumpable_levels() == 0) {
        // Find smallest non-zero column
        while (as_tbs.col(non_zero_col).none()) {
            ++non_zero_col;
        }

        algebra::Coefficients as_tbs_first = as_tbs.pop_first_row();

        // Note: reason does not belong to trail, since the negation of one of its equations
        // is in the trail. Thus, the conversion loses the +1 on that equation.
        std::size_t index = as_tbs.cols() - 1 - non_zero_col;
        std::optional<heuristics::ClauseId> reason_rs_id = trail.reasons()[index];
        if (reason_ids && reason_rs_id.has_value()) {
            reason_ids->push_back(reason_rs_id.value());
        }
        algebra::RowSpace reason_rs = get_clause(reason_rs_id.value());
        dec_proc->update_with_resolved_clause(reason_rs_id.value());
        algebra::TrailBasisSpace reason = algebra::TrailBasisSpace::from_RowSpace(reason_rs, trail);
        algebra::Coefficients reason_first = reason.pop_first_row();

        // Add reason to as_tbs
        as_tbs.add_row(as_tbs_first + reason_first + algebra::Coefficients::unit(as_tbs_first.size(), 0));

        for (algebra::Coefficients row : reason) {
            as_tbs.add_row(row);
        }

        dec_proc->update_with_critical_equation(trail.span().row(trail.order_map()[index]));

        for (std::size_t col = 0; col <= non_zero_col; ++col) {
            assert(as_tbs.col(col).none());
        }
    }

    return std::make_pair(as_tbs.to_RowSpace(), as_tbs.jumpable_levels());
}

Result Solver::solve(std::atomic<bool>& stop_flag) {
    while (stop_flag.load() == false) {
        // Run unit propagation
        bool conflict = false;
        while (!conflict) {
            auto propagate_result = propagation.unit_propagate_once(*clause_sel, trail, eqn_false, num_attempt_props, num_watch_updates);
            if (propagate_result) {
                ++num_propagations;
                auto [eqn, reason] = *propagate_result;
                trail.span().reduce(eqn);
                trail.learn(eqn, reason);
                clause_sel->update_with_propagation(eqn, trail);
                assert(working_eqn.size() == eqn_false.size());
                working_eqn.unsafe_copy(eqn_false);
                trail.span().reduce(working_eqn);
                conflict = working_eqn.none();
            } else {
                break;
            }
        }

        // Check for conflict
        if (conflict) {
            ++num_conflicts;

            // Check for unsatisfiability
            if (level() == 0) {
                if (proof_logger) {
                    std::vector<heuristics::ClauseId> reason_ids;
                    reason_ids.reserve(trail.reasons().size());
                    for (const auto& reason : trail.reasons()) {
                        if (reason.has_value()) {
                            reason_ids.push_back(reason.value());
                        }
                    }
                    heuristics::ClauseId final_clause_id = clause_sel->get_num_clauses();
                    proof_logger->log_empty_clause(final_clause_id, reason_ids);
                }
                return ResultCase::Unsat {};
            }

            // Learn an asserting clause
            std::vector<heuristics::ClauseId> reason_ids;
            auto [asserting_clause, jumpable_levels] = find_asserting_clause(
                trail, proof_logger ? &reason_ids : nullptr);

            dec_proc->update_with_asserting_clause(asserting_clause);

            assert((asserting_clause.rows() != 1) || (jumpable_levels == trail.size() - 1));
            const bool do_restart = restart_policy.should_restart_after_conflict();
            std::size_t levels_removed = jumpable_levels;
            if (do_restart) {
                levels_removed = trail.size() - 1;
                trail.erase(trail.begin() + 1, trail.end());
                restart_policy.note_restart();
                ++num_restarts;
            } else {
                // Jump back in the trail
                trail.erase(trail.end() - jumpable_levels, trail.end());
            }
            clause_sel->add_asserting_clause(asserting_clause, trail, levels_removed);

            heuristics::ClauseId new_clause_id = clause_sel->get_num_clauses() - 1;
            dec_proc->update_with_new_clause(asserting_clause, new_clause_id);
            if (proof_logger) {
                std::reverse(reason_ids.begin(), reason_ids.end());
                proof_logger->log_learned_clause(new_clause_id, asserting_clause, reason_ids);
            }
        } else {
            // Check for satisfiability
            if (trail.span().rows() == num_vars) {
                algebra::RowSpace span_without_false = trail.span();
                span_without_false.pop_last_row();
                return ResultCase::Sat {span_without_false.get_unique_solution()};
            }

            // Make a decision
            auto decision = dec_proc->make_decision(trail.span(), clause_sel->get_clauses());
            trail.span().reduce(decision);
            trail.learn(decision, std::nullopt);
            clause_sel->update_with_decision(decision, trail);
            clause_sel->update_with_decision_clause(dec_proc->decision_clause_hint());
        }
    }
    return ResultCase::Timeout {};
}

void Solver::print_clauses(const PrintMode mode) const {
    std::cout << "Solver with " << num_vars << " variables, " << get_num_clauses() << " clauses:" << std::endl;
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

    for (std::size_t i = 0; i < trail.span().rows(); ++i) {
        algebra::Equation eqn = trail.span().row(trail.order_map()[i]);
        std::cout << "Learning " << eqn.to_string(mode) << std::endl;
        if (trail.reasons()[i]) {
            std::cout << "Reason: " << std::endl << get_clauses()[*trail.reasons()[i]].to_string(mode) << std::endl;
        } else {
            std::cout << "Reason: decision" << std::endl;
        }
    }
}

Solver Solver::from_DIMACS(const std::string& filename,
                           const heuristics::DecisionProcedureName dpn,
                           const heuristics::ClauseSelectorName csn,
                           const heuristics::DecisionPhase phase,
                           std::shared_ptr<io::ProofLogger> proof_logger,
                           const RestartConfig& restart_config) {
    io::ParsedDimacs parsed = io::parse_dimacs_file(filename);

    std::shared_ptr<heuristics::DecisionProcedure> dec_proc;
    std::shared_ptr<heuristics::ClauseSelector> clause_sel;

    dec_proc = heuristics::create_decision_procedure(dpn, parsed.num_vars, phase);
    clause_sel = heuristics::create_clause_selector(csn, parsed.num_vars);

    Solver solver{parsed.num_vars, std::move(clause_sel), std::move(dec_proc), std::move(proof_logger),
                  restart_config};

    for (const auto& clause : parsed.clauses) {
        solver.clause_sel->add_starting_clause(clause);
        heuristics::ClauseId new_clause_id = solver.clause_sel->get_num_clauses() - 1;
        solver.dec_proc->update_with_new_clause(clause, new_clause_id);
    }

    return solver;
}

} // namespace search
