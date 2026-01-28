#include <fstream>
#include <cassert>
#include "Solver.h"
#include "TrailBasisSpace.h"

std::optional<Equation> Solver::unit_prop_one_clause(ClauseId id) {
    Clause& clause = get_clause(id);

    // NOTE: Upon removing this check, unit propagation takes significantly longer?

    // // If clause is satisfied, don't do anything
    // working_eqn.unsafe_copy(clause.row(clause.true_witness));
    // trail.span().reduce(working_eqn);
    // working_eqn.flip(num_vars);
    // if (working_eqn.none()) {
    //     return std::nullopt;
    // } else {
    //     // Check if clause is satisfied
    //     bool found = false;
    //     for (std::size_t i = 0; i < clause.rows(); ++i) {
    //         working_eqn.unsafe_copy(clause.row(i));
    //         trail.span().reduce(working_eqn);
    //         working_eqn.flip(num_vars);s
    //         if (working_eqn.none()) {
    //             clause.true_witness = i;
    //             found = true;
    //             break;
    //         }
    //     }
    //     if (found) {
    //         return std::nullopt;
    //     }
    // }

    // If clause is unit, conflict/propagate are the same
    if (clause.rows() == 1) {
        working_eqn.unsafe_copy(clause.row(0));
        trail.span().reduce(working_eqn);
        working_eqn.flip(num_vars);
        if (working_eqn.none()) {
            return std::nullopt;
        } else {
            return working_eqn;
        }
    }

    // Otherwise, check both watched equations
    working_eqn.unsafe_copy(clause.row(clause.watched0));
    trail.span().reduce(working_eqn);
    bool first_is_zero = working_eqn.none();

    working_eqn2.unsafe_copy(clause.row(clause.watched1));
    trail.span().reduce(working_eqn2);
    bool second_is_zero = working_eqn2.none();

    bool sum_is_zero = (working_eqn == working_eqn2);

    if (!first_is_zero && !second_is_zero && !sum_is_zero) {
        // No propagation
        return std::nullopt;
    } else if (first_is_zero && second_is_zero) {
        // Find two new watched literals
        // If cannot find any, propagate eqn_false
        // If can only find one, propagate the non-zero watch

        std::size_t old_watched0 = clause.watched0;

        // Find first new equation to watch
        std::size_t i = 0;
        bool found = false;
        for (; i < clause.rows(); ++i) {
            if (i == clause.watched0 || i == clause.watched1) continue;
            working_eqn.unsafe_copy(clause.row(i));
            trail.span().reduce(working_eqn);
            if (!working_eqn.none()) {
                clause.watched0 = i;
                // working_eqn is exactly trail.span().reduce_copy(clause.row(clause.watched0))
                found = true;
                break;
            }
        }

        // If none found, return conflict
        if (!found) {
            return eqn_false;
        } else {
            ++num_watch_updates;
            clause_sel->update_with_new_watch(id, old_watched0, WatchType::Zero, trail);
        }

        // Continue with second equation with same i
        std::size_t old_watched1 = clause.watched1;

        found = false;
        ++i;
        for (; i < clause.rows(); ++i) {
            if (i == clause.watched1) continue;
            working_eqn2.unsafe_copy(clause.row(i));
            trail.span().reduce(working_eqn2);
            if (!working_eqn2.none() && (working_eqn2 != working_eqn)) {
                clause.watched1 = i;
                found = true;
                break;
            }
        }

        // If none found, learn clause.row(clause.watched0) + 1
        if (!found) { 
            // working_eqn still holds trail.span().reduce_copy(clause.row(clause.watched0))
            working_eqn.flip(num_vars);
            // At this point, eqn may be the zero vector if the clause is true
            if (working_eqn.none()) {
                return std::nullopt;
            } else {
                return working_eqn;
            }
        } else {
            // Update watchlists
            ++num_watch_updates;
            clause_sel->update_with_new_watch(id, old_watched1, WatchType::One, trail);
            return std::nullopt;
        }
    } else {
        // Find one new watched literal
        // If cannot find, propagate the non-zero watch

        Equation& non_zero_ref = first_is_zero ? working_eqn2 : working_eqn;
        Equation& working_ref = first_is_zero ? working_eqn : working_eqn2;
        std::size_t& changing_watch = first_is_zero ? clause.watched0 : clause.watched1;

        std::size_t old_watch = changing_watch;

        // Find a new equation to watch
        bool found = false;
        for (std::size_t i = 0; i < clause.rows(); ++i) {
            if (i == clause.watched0 || i == clause.watched1) continue;
            working_ref.unsafe_copy(clause.row(i));
            trail.span().reduce(working_ref);
            if (!working_ref.none() && (working_ref != non_zero_ref)) {
                changing_watch = i;
                found = true;
                break;
            }
        }

        // If none found, learn clause.row(clause.watched0) + 1
        if (!found) { 
            // working_eqn still holds trail.span().reduce_copy(clause.row(clause.watched0))
            non_zero_ref.flip(num_vars);
            // At this point, eqn may be the zero vector iff the clause is true
            if (non_zero_ref.none()) {
                return std::nullopt;
            } else {
                return non_zero_ref;
            }
        } else {
            // Update watchlists
            ++num_watch_updates;
            clause_sel->update_with_new_watch(id, old_watch, first_is_zero ? WatchType::Zero : WatchType::One, trail);
            return std::nullopt;
        }
    }
}

std::optional<std::pair<Equation, ClauseId>> Solver::unit_propagate_once() {
    while (true) {
        auto maybe_clause = clause_sel->next();
        if (!maybe_clause.has_value()) {
            return std::nullopt;
        } else {
            ClauseId id = maybe_clause.value();
            ++num_attempt_props;
            auto result = unit_prop_one_clause(id);
            if (result.has_value()) {
                return std::pair<Equation, ClauseId>{result.value(), id};
            }
        }
    }
}

std::pair<Clause, std::size_t> Solver::find_asserting_clause(const Trail& trail) {
    // Fact: last_reason should never be std::nullopt here
    std::optional<ClauseId> last_reason_id = trail.reasons()[trail.span().rows() - 1];
    Clause& last_reason = get_clause(last_reason_id.value());
    TrailBasisSpace as_tbs = TrailBasisSpace::from_RowSpace(last_reason, trail);

    std::size_t non_zero_col = 0;
    while (as_tbs.jumpable_levels() == 0) {
        // Find smallest non-zero column
        while (as_tbs.col(non_zero_col).none()) {
            ++non_zero_col;
        }

        Coefficients as_tbs_first = as_tbs.pop_first_row();
        
        // Note: reason does not belong to trail, since the negation of one of its equations
        // is in the trail. Thus, the conversion loses the +1 on that equation.
        std::size_t index = as_tbs.cols() - 1 - non_zero_col;
        std::optional<ClauseId> reason_rs_id = trail.reasons()[index];
        RowSpace reason_rs = get_clause(reason_rs_id.value());
        TrailBasisSpace reason = TrailBasisSpace::from_RowSpace(reason_rs, trail);
        Coefficients reason_first = reason.pop_first_row();

        // Add reason to as_tbs
        as_tbs.add_row(as_tbs_first + reason_first + Coefficients::unit(as_tbs_first.size(), 0));

        for (Coefficients row : reason) {
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
            auto propagate_result = unit_propagate_once();
            if (propagate_result) {
                ++num_propagations;
                auto [eqn, reason] = *propagate_result;
                trail.span().reduce(eqn);
                trail.learn(eqn, reason);
                clause_sel->update_with_propagation(eqn, trail);
                working_eqn = eqn_false;
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
                RowSpace span_without_false = trail.span();
                span_without_false.pop_last_row();
                return ResultCase::Sat {span_without_false.get_unique_solution()};
            }

            // Make a decision
            auto decision = dec_proc->make_decision(trail.span());
            trail.span().reduce(decision);
            trail.learn(decision, std::nullopt);
            clause_sel->update_with_decision(decision, trail);
        }
    }
    return ResultCase::Timeout {};
}

void Solver::print_clauses(const PrintMode mode) const {
    std::cout << "Solver with " << num_vars << " variables, " << get_num_clauses() << " clauses:" << std::endl;
    const std::vector<Clause>& clauses = get_clauses();
    for (const auto& clause : clauses) {
        std::cout << clause.to_string(mode) << std::endl;
    }
}

void Solver::print_trail(const EquationPrintMode eqn_mode) const {
    PrintMode mode{eqn_mode, ClausePrintMode::as_rowspace};

    std::cout << "Trail of length " << trail.span().rows() << ":" << std::endl;
    Clause trail_as_clause = trail.span();
    std::cout << "Final row space " << std::endl << trail_as_clause.to_string(mode) << std::endl;

    for (std::size_t i = 0; i < trail.span().rows(); ++i) {
        Equation eqn = trail.span().row(trail.order_map()[i]);
        std::cout << "Learning " << eqn.to_string(mode) << std::endl;
        if (trail.reasons()[i]) {
            std::cout << "Reason: " << std::endl << get_clauses()[*trail.reasons()[i]].to_string(mode) << std::endl;
        } else {
            std::cout << "Reason: decision" << std::endl;
        }
    }
}

Solver Solver::from_DIMACS(const std::string& filename, const DecisionProcedureName dpn) {
    std::ifstream file{filename};
    std::string line;

    if (!file.is_open()) {
        throw std::runtime_error("Unable to open file: " + filename);
    }
    
    // Skip comments and empty lines
    while (std::getline(file, line) && (line.empty() || line[0] == 'c')) {}

    // Get number of variables from header
    if (line[0] != 'p') {
        throw std::runtime_error("Invalid DIMACS header: " + line);
    }

    std::istringstream iss{line};
    std::size_t num_vars;
    std::size_t tmp_num_clauses;
    std::string tmp;
    iss >> tmp >> tmp >> num_vars >> tmp_num_clauses;

    // if (tmp_num_clauses >= 150) {
    //     throw std::runtime_error("Too many clauses: " + std::to_string(tmp_num_clauses));
    // }

    std::unique_ptr<DecisionProcedure> dec_proc;

    switch (dpn) {
        case DecisionProcedureName::fixed_unit:
            dec_proc = std::make_unique<FixedUnit>(num_vars);
            break;
        case DecisionProcedureName::random_unit:
            dec_proc = std::make_unique<RandomUnit>(num_vars);
            break;
        case DecisionProcedureName::random_eq:
            dec_proc = std::make_unique<RandomEq>(num_vars);
            break;
        case DecisionProcedureName::vsids_unit:
            dec_proc = std::make_unique<VsidsUnit>(num_vars);
            break;
    }

    Solver solver{num_vars, std::move(dec_proc)};

    while (std::getline(file, line)) {
        if (line.empty() || line[0] == 'c') {
            continue; // Skip comments and empty lines
        }
        
        // Insert whitespace around parentheses
        for (std::string target : {"(", ")"}) {
            size_t pos{0};
            while ((pos = line.find(target, pos)) != std::string::npos) {
                line.replace(pos, target.length(), " " + target + " ");
                pos += target.length() + 2;
            }
        }
        
        Clause clause(num_vars);
        std::istringstream iss{line};
        std::string token;

        while (iss >> token) {
            if (token == "0") {
                break;
            }

            Equation equation{solver.eqn_true}; 

            if (token == "(") { // Process equation-type term
                bool is_neg{false};
                while (true) {
                    iss >> token;
                    if (token == ")") {
                        break;
                    }
                    if (token == "0" || token == "(") {
                        throw std::runtime_error("Found invalid token inside equation");
                    }
                    int literal{std::stoi(token)};
                    // Negative literals mean negated variables
                    if (literal < 0) {
                        is_neg = !is_neg;
                        literal = std::abs(literal);
                    }
                    equation.set(literal - 1);
                }
                // Negative means the original intent is = 0. In the annihilator representation
                // of clauses, this is converted to = 1, thus we set the last bit.
                if (is_neg) {
                    equation.set(num_vars);
                }
            } else { // Process legacy DIMACS-type term
                int literal{std::stoi(token)};
                if (literal < 0) {
                    literal = std::abs(literal);
                    equation.set(num_vars);
                }
                equation.set(literal - 1);
            }

            clause.add_row(equation);
        }
        solver.clause_sel->add_starting_clause(clause);
        solver.dec_proc->update_with_starting_clause(clause);
    }
    file.close();
    return solver;
}

