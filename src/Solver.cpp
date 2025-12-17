#include <fstream>
#include <cassert>
#include "Solver.h"
#include "TrailBasisSpace.h"

void Solver::add_clause(Clause& clause) {
    if (clause.rows() == 0) { 
        return;
    }

    watchlist.by_leading_term[clause.row(clause.watched0).first_set()].push_back(clauses.size());
    if (clause.rows() > 1) {
        watchlist.by_leading_term[clause.row(clause.watched1).first_set()].push_back(clauses.size());
    }
    clauses.push_back(clause);
}

std::optional<Equation> Solver::unit_prop_one_clause(Clause& clause) {
    // NOTE: Upon removing this check, unit propagation takes significantly longer?

    // If clause is satisfied, don't do anything
    Equation witness = trail.span().reduce_copy(clause.row(clause.true_witness));
    witness.flip(num_vars);
    if (witness.none()) {
        return std::nullopt;
    } else {
        // Check if clause is satisfied
        bool found = false;
        for (std::size_t i = 0; i < clause.rows(); ++i) {
            witness = trail.span().reduce_copy(clause.row(i));
            witness.flip(num_vars);
            if (witness.none()) {
                clause.true_witness = i;
                found = true;
                break;
            }
        }
        if (found) {
            return std::nullopt;
        }
    }

    // If clause is unit, conflict/propagate are the same
    if (clause.rows() == 1) {
        Equation eqn = trail.span().reduce_copy(clause.row(0));
        eqn.flip(num_vars);
        if (eqn.none()) {
            return std::nullopt;
        } else {
            return eqn;
        }
    }

    // Otherwise, check watched equations
    Equation reduced0 = trail.span().reduce_copy(clause.row(clause.watched0));
    if (reduced0.none()) {
        // Find a new equation to watch
        bool found = false;
        for (std::size_t i = 0; i < clause.rows(); ++i) {
            if (i == clause.watched0) continue;
            Equation reduced = trail.span().reduce_copy(clause.row(i));
            if (!reduced.none()) {
                clause.watched0 = i;
                
                reduced0 = trail.span().reduce_copy(clause.row(clause.watched0));
                found = true;
                break;
            }
        }
        // If none found, return conflict
        if (!found) {
            return eqn_false;
        }
    }
    // Postcondition: clause.row(clause.watched0) is non-zero w.r.t. current trail

    // Do the same things with clause.row(clause.watched1), but reduce using clause.row(clause.watched0).
    Equation reduced1 = trail.span().reduce_with_extra_copy(clause.row(clause.watched1), reduced0);
    if (reduced1.none()) {
        // Find a new equation to watch
        bool found = false;
        for (std::size_t i = 0; i < clause.rows(); ++i) {
            if (i == clause.watched0 || i == clause.watched1) continue;
            Equation reduced = trail.span().reduce_with_extra_copy(clause.row(i), reduced0);
            if (!reduced.none()) {
                clause.watched1 = i;
                found = true;
                break;
            }
        }
        // If none found, learn clause.row(clause.watched0) + 1
        if (!found) {
            Equation eqn = trail.span().reduce_copy(clause.row(clause.watched0));
            eqn.flip(num_vars);
            // At this point, eqn may be the zero vector iff the clause is true
            if (eqn.none()) {
                return std::nullopt;
            } else {
                return eqn;
            }
        }
    }
    return std::nullopt;
}

std::optional<std::pair<Equation, std::reference_wrapper<Clause>>> Solver::unit_propagate_once() {
    std::size_t start = clause_idx;
    std::size_t end = start + clauses.size();
    for (std::size_t i = start; i < end; i++) {
        Clause& clause = clauses[i % clauses.size()];
        auto result = unit_prop_one_clause(clause);
        if (result.has_value()) {
            clause_idx = (i + 1) % clauses.size();
            return std::make_pair(result.value(), std::ref(clause));
        }
    }
    // No deductions possible
    return std::nullopt;
}

std::pair<Clause, std::size_t> Solver::find_asserting_clause(const Trail& trail) {
    // Fact: last_reason should never be std::nullopt here
    std::optional<std::reference_wrapper<Clause>> last_reason = trail.reasons()[trail.span().rows() - 1];
    TrailBasisSpace as_tbs = TrailBasisSpace::from_RowSpace(last_reason->get(), trail);

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
        RowSpace reason_rs = *trail.reasons()[index];
        TrailBasisSpace reason = TrailBasisSpace::from_RowSpace(reason_rs, trail);
        Coefficients reason_first = reason.pop_first_row();

        // Add reason to as_tbs
        as_tbs.add_row(as_tbs_first + reason_first + Coefficients::unit(as_tbs_first.size(), 0));

        for (Coefficients row : reason) {
            as_tbs.add_row(row);
        }

        std::visit([&trail, index](auto& dec_proc_concrete) {
            dec_proc_concrete.update_with_critical_equation(trail.span().row(trail.order_map()[index]));
        }, dec_proc);

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
                auto [eqn, reason] = *propagate_result;
                trail.learn(eqn, reason);
                bit::vector<> tmp = trail.span().reduce_copy(eqn_false);
                conflict = tmp.none();
            } else {
                break;
            }
        }

        // Check for conflict
        if (conflict) {
            // Check for unsatisfiability
            if (level() == 0) {
                return ResultCase::Unsat {};
            }

            // Learn an asserting clause
            auto [asserting_clause, jumpable_levels] = find_asserting_clause(trail);

            std::visit([&asserting_clause](auto& dec_proc_concrete) {
                dec_proc_concrete.update_with_asserting_clause(asserting_clause);
            }, dec_proc);

            add_clause(asserting_clause);

            // Jump back in the trail
            trail.resize(trail.size() - jumpable_levels);
        } else {
            // Check for satisfiability
            if (trail.span().rows() == num_vars) {
                RowSpace span_without_false = trail.span();
                span_without_false.pop_last_row();
                return ResultCase::Sat {span_without_false.get_unique_solution()};
            }

            // Make a decision
            std::visit([this](auto& dec_proc_concrete) {
                auto decision = dec_proc_concrete.make_decision(trail.span());
                trail.learn(decision, std::nullopt);
            }, dec_proc);
        }
    }
    return ResultCase::Timeout {};
}






void Solver::print_clauses(const PrintMode mode) const {
    std::cout << "Solver with " << num_vars << " variables, " << clauses.size() << " clauses:" << std::endl;
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
            std::cout << "Reason: " << std::endl << (*trail.reasons()[i]).get().to_string(mode) << std::endl;
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
    std::string tmp;
    iss >> tmp >> tmp >> num_vars;

    DecisionProcedure dec_proc = [&] {
        switch (dpn) {
            case DecisionProcedureName::fixed_unit:
                return DecisionProcedure{FixedUnit(num_vars)};
            case DecisionProcedureName::random_unit:
                return DecisionProcedure{RandomUnit(num_vars)};
            case DecisionProcedureName::random_eq:
                return DecisionProcedure{RandomEq(num_vars)};
            case DecisionProcedureName::vsids_unit:
                return DecisionProcedure{VsidsUnit(num_vars)};
        }
    }();


    Solver solver{num_vars, dec_proc};

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
        
        Clause clause;
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
        solver.add_clause(clause);
    }
    file.close();
    return solver;
}

