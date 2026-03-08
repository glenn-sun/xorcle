#include <algorithm>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <numeric>
#include <stdexcept>

#include "search/Solver.h"

#include "algebra/TrailBasisSpace.h"
#include "heuristics/ClauseBasedDecisions.h"
#include "heuristics/Factory.h"
#include "io/DimacsParser.h"
#include "util/DecisionUtils.h"

namespace search {
namespace {

algebra::Equation lift_equation_with_map(const algebra::Equation& eqn_active,
                                         const std::vector<std::size_t>& active_to_original,
                                         std::size_t original_num_vars) {
    if (eqn_active.size() != active_to_original.size() + 1) {
        throw std::runtime_error("lift_equation_with_map: width mismatch");
    }

    algebra::Equation lifted{original_num_vars};
    for (std::size_t i = 0; i < active_to_original.size(); ++i) {
        if (eqn_active.element(i)) {
            lifted.set(active_to_original[i]);
        }
    }
    if (eqn_active.element(active_to_original.size())) {
        lifted.set(original_num_vars);
    }
    return lifted;
}

std::optional<std::size_t> find_row_index(const algebra::Clause& clause, const algebra::Equation& eqn) {
    for (std::size_t i = 0; i < clause.rows(); ++i) {
        if (clause.row(i) == eqn) {
            return i;
        }
    }
    return std::nullopt;
}

std::size_t first_distinct_index(std::size_t rows, std::size_t avoid) {
    for (std::size_t i = 0; i < rows; ++i) {
        if (i != avoid) {
            return i;
        }
    }
    return 0;
}

void assign_transformed_clause_metadata(algebra::Clause& rewritten,
                                        const algebra::Clause& old_clause,
                                        const algebra::RowSpace& trail_span,
                                        const CompactionTransform& transform) {
    if (rewritten.rows() <= 1) {
        rewritten.watched0 = 0;
        rewritten.watched1 = 0;
        rewritten.true_witness = 0;
        rewritten.next_watch_search_start = 0;
        return;
    }

    auto project_row = [&](std::size_t idx) -> std::optional<algebra::Equation> {
        if (idx >= old_clause.rows()) {
            return std::nullopt;
        }
        algebra::Equation reduced = old_clause.row(idx);
        trail_span.reduce(reduced);
        return transform.project_equation(reduced);
    };

    std::optional<std::size_t> mapped_w0;
    std::optional<std::size_t> mapped_w1;
    std::optional<std::size_t> mapped_tw;

    if (auto row = project_row(old_clause.watched0); row.has_value()) {
        mapped_w0 = find_row_index(rewritten, *row);
    }
    if (auto row = project_row(old_clause.watched1); row.has_value()) {
        mapped_w1 = find_row_index(rewritten, *row);
    }
    if (auto row = project_row(old_clause.true_witness); row.has_value()) {
        mapped_tw = find_row_index(rewritten, *row);
    }

    std::size_t watched0 = mapped_w0.value_or(mapped_w1.value_or(0));
    if (watched0 >= rewritten.rows()) {
        watched0 = 0;
    }
    std::size_t watched1 = mapped_w1.value_or(first_distinct_index(rewritten.rows(), watched0));
    if (watched1 >= rewritten.rows() || watched1 == watched0) {
        watched1 = first_distinct_index(rewritten.rows(), watched0);
    }

    rewritten.watched0 = watched0;
    rewritten.watched1 = watched1;
    rewritten.true_witness = mapped_tw.value_or(watched0);
    if (rewritten.true_witness >= rewritten.rows()) {
        rewritten.true_witness = watched0;
    }
    rewritten.next_watch_search_start = old_clause.next_watch_search_start % rewritten.rows();
}

} // namespace

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
               const RestartConfig& restart_config,
               bool enable_compaction_in)
    : enable_compaction(enable_compaction_in),
      original_num_vars(num_vars),
      active_num_vars(num_vars),
      active_to_original(num_vars),
      trail(num_vars),
      clause_sel(std::move(clause_sel_in)),
      dec_proc(std::move(dec_proc)),
      propagation(num_vars),
      restart_policy(restart_config),
      proof_logger(std::move(proof_logger_in)),
      working_eqn(bit::vector<>::zeros(num_vars + 1)),
      eqn_false(algebra::Equation::unit(num_vars + 1, num_vars)) {
    std::iota(active_to_original.begin(), active_to_original.end(), 0);
}

Solver::Solver(std::size_t num_vars,
               std::shared_ptr<heuristics::DecisionProcedure> dec_proc,
               std::shared_ptr<io::ProofLogger> proof_logger_in,
               const RestartConfig& restart_config,
               bool enable_compaction_in)
    : Solver(num_vars, std::make_shared<heuristics::Cyclic>(num_vars), std::move(dec_proc),
             std::move(proof_logger_in), restart_config, enable_compaction_in) {}

std::size_t Solver::get_num_vars() const { return original_num_vars; }

std::size_t Solver::get_active_num_vars() const { return active_num_vars; }

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

algebra::Equation Solver::lift_active_equation_to_original(const algebra::Equation& eqn_active) const {
    return lift_equation_with_map(eqn_active, active_to_original, original_num_vars);
}

algebra::Clause Solver::build_proof_clause_from_active(const algebra::Clause& clause_active) const {
    algebra::Clause proof_clause(original_num_vars);
    for (std::size_t i = 0; i < clause_active.rows(); ++i) {
        proof_clause.add_row(lift_active_equation_to_original(clause_active.row(i)));
    }

    for (const CompactedUnitProofEntry& unit : compacted_units_for_proof) {
        proof_clause.add_row(unit.eqn_original);
    }

    proof_clause.watched0 = 0;
    proof_clause.watched1 = (proof_clause.rows() > 1) ? 1 : 0;
    proof_clause.true_witness = 0;
    proof_clause.next_watch_search_start = 0;
    return proof_clause;
}

bit::vector<> Solver::reconstruct_original_assignment(const bit::vector<>& active_assignment) const {
    bit::vector<> assignment = active_assignment;

    for (auto it = compaction_history.rbegin(); it != compaction_history.rend(); ++it) {
        const CompactionStep& step = *it;
        if (assignment.size() != step.transform.new_num_vars) {
            throw std::runtime_error("reconstruct_original_assignment: active width mismatch");
        }

        bit::vector<> old_values = bit::vector<>::zeros(step.transform.old_num_vars);
        for (std::size_t i = 0; i < step.transform.new_num_vars; ++i) {
            if (assignment.element(i)) {
                old_values.set(step.transform.new_to_old[i]);
            }
        }

        for (auto row_it = step.span_rows_old.rbegin(); row_it != step.span_rows_old.rend(); ++row_it) {
            const algebra::Equation& row = *row_it;
            std::size_t pivot = row.first_set();
            if (pivot == bit::vector<>::npos || pivot >= step.transform.old_num_vars) {
                continue;
            }

            bool value = row.element(step.transform.old_num_vars);
            for (std::size_t col = row.next_set(pivot); col != bit::vector<>::npos; col = row.next_set(col)) {
                if (col == step.transform.old_num_vars) {
                    continue;
                }
                value ^= old_values.element(col);
            }

            if (value) {
                old_values.set(pivot);
            } else {
                old_values.reset(pivot);
            }
        }

        assignment = std::move(old_values);
    }

    if (assignment.size() == original_num_vars) {
        return assignment;
    }

    bit::vector<> lifted = bit::vector<>::zeros(original_num_vars);
    if (assignment.size() == active_to_original.size()) {
        for (std::size_t i = 0; i < active_to_original.size(); ++i) {
            if (assignment.element(i)) {
                lifted.set(active_to_original[i]);
            }
        }
        return lifted;
    }

    throw std::runtime_error("reconstruct_original_assignment: failed to recover original assignment width");
}

bool Solver::maybe_compact_level0_before_decision() {
    if (level() != 0) {
        return false;
    }
    if (trail.span().rows() < bit::vector<>::bits_per_block) {
        return false;
    }
    if (active_num_vars == 0) {
        return false;
    }

    const bit::vector<>& pivots = trail.span().get_pivot_cols();
    if (pivots.size() != active_num_vars + 1) {
        return false;
    }
    if (pivots.element(active_num_vars)) {
        return false;
    }

    std::size_t pivot_count = 0;
    pivots.if_set_call([&](std::size_t col) {
        if (col < active_num_vars) {
            ++pivot_count;
        }
    });
    if (pivot_count == 0) {
        return false;
    }

    CompactionTransform transform = CompactionTransform::from_pivots(active_num_vars, pivots);
    if (transform.new_num_vars == active_num_vars) {
        return false;
    }

    const std::size_t num_clauses = clause_sel->get_num_clauses();
    std::vector<algebra::Clause> rewritten_clauses;
    rewritten_clauses.reserve(num_clauses);

    std::vector<std::uint8_t> next_active = clause_sel->active_flags();
    if (next_active.size() < num_clauses) {
        next_active.resize(num_clauses, 1);
    }

    util::ClauseSatisfactionWorkspace sat_ws(active_num_vars);
    algebra::Equation eqn_false_new = algebra::Equation::unit(transform.new_num_vars + 1, transform.new_num_vars);

    bool has_invalid_projection = false;
    const std::vector<algebra::Clause>& old_clauses = clause_sel->get_clauses();
    for (heuristics::ClauseId id = 0; id < num_clauses; ++id) {
        const algebra::Clause& old_clause = old_clauses[id];
        const bool was_active = clause_sel->is_clause_active(id);

        bool inactive_now = !was_active;
        if (was_active) {
            inactive_now = util::clause_satisfied_by_trail(trail.span(), old_clause, sat_ws);
        }

        algebra::Clause rewritten(transform.new_num_vars);
        if (inactive_now) {
            rewritten.add_row(eqn_false_new);
            rewritten.watched0 = 0;
            rewritten.watched1 = 0;
            rewritten.true_witness = 0;
            rewritten.next_watch_search_start = 0;
            next_active[id] = 0;
            rewritten_clauses.push_back(std::move(rewritten));
            continue;
        }

        for (std::size_t row = 0; row < old_clause.rows(); ++row) {
            algebra::Equation reduced = old_clause.row(row);
            trail.span().reduce(reduced);
            rewritten.add_row(transform.project_equation(reduced));
        }

        if (rewritten.rows() == 0) {
            has_invalid_projection = true;
            break;
        }

        assign_transformed_clause_metadata(rewritten, old_clause, trail.span(), transform);
        next_active[id] = 1;
        rewritten_clauses.push_back(std::move(rewritten));
    }

    if (has_invalid_projection || rewritten_clauses.size() != num_clauses) {
        return false;
    }

    CompactionStep step;
    step.transform = transform;
    step.active_to_original_before = active_to_original;
    step.eliminated_original_vars.reserve(pivot_count);
    step.span_rows_old.reserve(trail.span().rows());
    step.lifted_span_rows_original.reserve(trail.span().rows());

    for (std::size_t i = 0; i < active_num_vars; ++i) {
        if (transform.is_pivot_old(i)) {
            step.eliminated_original_vars.push_back(active_to_original[i]);
        }
    }

    for (std::size_t row = 0; row < trail.span().rows(); ++row) {
        const algebra::Equation& eqn_active = trail.span().row(row);
        step.span_rows_old.push_back(eqn_active);
        step.lifted_span_rows_original.push_back(
            lift_equation_with_map(eqn_active, step.active_to_original_before, original_num_vars));
    }

    std::vector<CompactedUnitProofEntry> new_proof_units;
    new_proof_units.reserve(trail.reasons().size());
    auto seen_unit = [&](const algebra::Equation& eqn_original) {
        for (const CompactedUnitProofEntry& entry : compacted_units_for_proof) {
            if (entry.eqn_original == eqn_original) {
                return true;
            }
        }
        for (const CompactedUnitProofEntry& entry : new_proof_units) {
            if (entry.eqn_original == eqn_original) {
                return true;
            }
        }
        return false;
    };
    for (std::size_t i = 0; i < trail.reasons().size(); ++i) {
        std::size_t row_idx = trail.order_map()[i];
        const algebra::Equation& eqn_active = trail.span().row(row_idx);
        algebra::Equation eqn_original =
            lift_equation_with_map(eqn_active, step.active_to_original_before, original_num_vars);
        if (seen_unit(eqn_original)) {
            continue;
        }
        new_proof_units.push_back(CompactedUnitProofEntry{eqn_original, trail.reasons()[i]});
    }

    for (heuristics::ClauseId id = 0; id < num_clauses; ++id) {
        get_clause(id) = std::move(rewritten_clauses[id]);
        clause_sel->set_clause_active(id, next_active[id] != 0);
    }

    std::vector<std::size_t> next_active_to_original(transform.new_num_vars);
    for (std::size_t i = 0; i < transform.new_num_vars; ++i) {
        next_active_to_original[i] = active_to_original[transform.new_to_old[i]];
    }

    active_num_vars = transform.new_num_vars;
    active_to_original = std::move(next_active_to_original);
    compaction_history.push_back(std::move(step));
    compacted_units_for_proof.insert(compacted_units_for_proof.end(),
                                     std::make_move_iterator(new_proof_units.begin()),
                                     std::make_move_iterator(new_proof_units.end()));

    propagation.reconfigure(active_num_vars);
    eqn_false = algebra::Equation::unit(active_num_vars + 1, active_num_vars);
    working_eqn.resize(active_num_vars + 1);
    working_eqn.reset();
    trail = Trail(active_num_vars);
    clause_sel->update_with_compaction(transform, trail);
    dec_proc->update_with_compaction(transform, trail.span(), clause_sel->get_clauses(), clause_sel->active_flags());

    return true;
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
    proof_clause_id_allocator = std::max(proof_clause_id_allocator, clause_sel->get_num_clauses());

    while (stop_flag.load() == false) {
        // Run unit propagation
        bool conflict = false;
        while (!conflict) {
            auto propagate_result = propagation.unit_propagate_once(*clause_sel, trail, eqn_false, num_attempt_props, num_watch_updates);
            if (propagate_result) {
                ++num_propagations;
                auto [eqn, reason] = *propagate_result;
                (void)trail.span().partial_reduce(eqn);
                trail.learn(eqn, reason);
                clause_sel->update_with_propagation(eqn, trail);
                assert(working_eqn.size() == eqn_false.size());
                working_eqn.unsafe_copy(eqn_false);
                conflict = (trail.span().partial_reduce(working_eqn) == bit::vector<>::npos);
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
                    proof_clause_id_allocator = std::max(proof_clause_id_allocator, clause_sel->get_num_clauses());

                    std::vector<heuristics::ClauseId> conflict_reason_ids;
                    conflict_reason_ids.reserve(trail.reasons().size());
                    for (const auto& reason : trail.reasons()) {
                        if (reason.has_value()) {
                            conflict_reason_ids.push_back(reason.value());
                        }
                    }

                    if (!compacted_units_for_proof.empty()) {
                        algebra::Clause guard_clause(original_num_vars);
                        for (const CompactedUnitProofEntry& unit : compacted_units_for_proof) {
                            guard_clause.add_row(unit.eqn_original);
                        }
                        guard_clause.watched0 = 0;
                        guard_clause.watched1 = (guard_clause.rows() > 1) ? 1 : 0;
                        guard_clause.true_witness = 0;
                        guard_clause.next_watch_search_start = 0;

                        heuristics::ClauseId guard_clause_id = proof_clause_id_allocator++;
                        proof_logger->log_learned_clause(guard_clause_id, guard_clause, conflict_reason_ids);

                        std::vector<heuristics::ClauseId> final_reason_ids;
                        final_reason_ids.reserve(compacted_units_for_proof.size() + 1);
                        for (const CompactedUnitProofEntry& unit : compacted_units_for_proof) {
                            if (unit.reason_id.has_value()) {
                                final_reason_ids.push_back(*unit.reason_id);
                            }
                        }
                        final_reason_ids.push_back(guard_clause_id);
                        heuristics::ClauseId empty_clause_id = proof_clause_id_allocator++;
                        proof_logger->log_empty_clause(empty_clause_id, final_reason_ids);
                    } else {
                        heuristics::ClauseId final_clause_id = proof_clause_id_allocator++;
                        proof_logger->log_empty_clause(final_clause_id, conflict_reason_ids);
                    }
                }
                return ResultCase::Unsat {};
            }

            // Learn an asserting clause
            std::vector<heuristics::ClauseId> reason_ids;
            auto [asserting_clause, jumpable_levels] = find_asserting_clause(
                trail, proof_logger ? &reason_ids : nullptr);
            heuristics::ClauseId new_clause_id = clause_sel->get_num_clauses();

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
            dec_proc->update_with_new_clause(asserting_clause, new_clause_id);
            if (proof_logger) {
                std::reverse(reason_ids.begin(), reason_ids.end());
                const bool needs_lift = (active_num_vars != original_num_vars) || !compacted_units_for_proof.empty();
                if (needs_lift) {
                    algebra::Clause proof_clause = build_proof_clause_from_active(asserting_clause);
                    proof_logger->log_learned_clause(new_clause_id, proof_clause, reason_ids);
                } else {
                    proof_logger->log_learned_clause(new_clause_id, asserting_clause, reason_ids);
                }
            }
            clause_sel->add_asserting_clause(std::move(asserting_clause), trail, levels_removed);
            proof_clause_id_allocator = std::max(proof_clause_id_allocator, clause_sel->get_num_clauses());
        } else {
            // Check for satisfiability
            if (trail.span().rows() == active_num_vars) {
                bit::vector<> active_assignment = trail.span().get_unique_solution();
                bit::vector<> original_assignment = reconstruct_original_assignment(active_assignment);
                return ResultCase::Sat {std::move(original_assignment)};
            }

            // Compact at level 0 right before making a decision.
            if (enable_compaction && maybe_compact_level0_before_decision()) {
                continue;
            }

            // Make a decision
            auto decision = dec_proc->make_decision(trail.span(), clause_sel->get_clauses());
            (void)trail.span().partial_reduce(decision);
            trail.learn(decision, std::nullopt);
            clause_sel->update_with_decision(decision, trail);
            clause_sel->update_with_decision_clause(dec_proc->decision_clause_hint());
        }
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
                           const heuristics::CVsidsConfig& cvsids_config,
                           std::shared_ptr<io::ProofLogger> proof_logger,
                           const RestartConfig& restart_config,
                           bool enable_compaction) {
    io::ParsedDimacs parsed = io::parse_dimacs_file(filename);

    std::shared_ptr<heuristics::DecisionProcedure> dec_proc;
    std::shared_ptr<heuristics::ClauseSelector> clause_sel;

    dec_proc = heuristics::create_decision_procedure(dpn, parsed.num_vars, phase, cvsids_config);
    clause_sel = heuristics::create_clause_selector(csn, parsed.num_vars);

    Solver solver{parsed.num_vars, std::move(clause_sel), std::move(dec_proc), std::move(proof_logger),
                  restart_config, enable_compaction};

    for (auto& clause : parsed.clauses) {
        heuristics::ClauseId new_clause_id = solver.clause_sel->get_num_clauses();
        solver.dec_proc->update_with_new_clause(clause, new_clause_id);
        solver.clause_sel->add_starting_clause(std::move(clause));
    }

    solver.proof_clause_id_allocator = solver.clause_sel->get_num_clauses();
    return solver;
}

} // namespace search
