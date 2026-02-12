#include <cassert>

#include "search/Solver.h"

#include "algebra/TrailBasisSpace.h"
#include "heuristics/CVsids.h"
#include "heuristics/Factory.h"
#include "io/DimacsParser.h"

namespace search {

std::pair<algebra::Clause, std::size_t> Solver::find_asserting_clause(const Trail& trail) {
    // Fact: last_reason should never be std::nullopt here
    std::optional<heuristics::ClauseId> last_reason_id = trail.reasons()[trail.span().rows() - 1];
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
                return ResultCase::Unsat {};
            }

            // Learn an asserting clause
            auto [asserting_clause, jumpable_levels] = find_asserting_clause(trail);

            dec_proc->update_with_asserting_clause(asserting_clause);

            if (asserting_clause.rows() == 1) {
                // Restart the trail
                trail.erase(trail.begin() + 1, trail.end());
                clause_sel->add_asserting_clause(asserting_clause, trail, trail.size() - 1);
            } else {
                // Jump back in the trail
                trail.erase(trail.end() - jumpable_levels, trail.end());
                clause_sel->add_asserting_clause(asserting_clause, trail, jumpable_levels);
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

Solver Solver::from_DIMACS(const std::string& filename, const heuristics::DecisionProcedureName dpn, const heuristics::ClauseSelectorName csn) {
    io::ParsedDimacs parsed = io::parse_dimacs_file(filename);

    std::shared_ptr<heuristics::DecisionProcedure> dec_proc;
    std::shared_ptr<heuristics::ClauseSelector> clause_sel;

    if (dpn == heuristics::DecisionProcedureName::cvsids_sum || csn == heuristics::ClauseSelectorName::cvsids_sum) {
        auto shared_state = std::make_shared<heuristics::CVsidsState>(parsed.num_vars);
        dec_proc = std::make_shared<heuristics::CVsidsSumDecision>(parsed.num_vars, shared_state);
        clause_sel = std::make_shared<heuristics::CVsidsSelector>(parsed.num_vars, shared_state);
    } else if (dpn == heuristics::DecisionProcedureName::cvsids || csn == heuristics::ClauseSelectorName::cvsids) {
        auto shared_state = std::make_shared<heuristics::CVsidsState>(parsed.num_vars);
        dec_proc = std::make_shared<heuristics::CVsidsDecision>(parsed.num_vars, shared_state);
        clause_sel = std::make_shared<heuristics::CVsidsSelector>(parsed.num_vars, shared_state);
    } else {
        dec_proc = heuristics::create_decision_procedure(dpn, parsed.num_vars);
        clause_sel = heuristics::create_clause_selector(csn, parsed.num_vars);
    }

    Solver solver{parsed.num_vars, std::move(clause_sel), std::move(dec_proc)};

    for (const auto& clause : parsed.clauses) {
        solver.clause_sel->add_starting_clause(clause);
        solver.dec_proc->update_with_starting_clause(clause);
    }

    return solver;
}

} // namespace search
