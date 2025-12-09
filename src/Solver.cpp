#include <fstream>
#include <sstream>
#include <cassert>
#include <cstdlib>
#include <ctime>
#include "Solver.h"

bit::vector<> RowSpace::get_unique_solution() const {
    bit::matrix<> M_copy = M;
    M_copy.to_reduced_echelon_form();
    return M_copy.col(M_copy.cols() - 1);
}

TrailBasisSpace TrailBasisSpace::from_RowSpace(const RowSpace& rs, const Trail& trail) {
    TrailBasisSpace tbs(trail);
    for (const Equation& eqn : rs) {
        // Coefficients in representation order
        Coefficients coeffs;
        trail.span().reduce_copy(eqn, &coeffs);

        // Convert to trail basis order
        std::size_t m = coeffs.size();
        Coefficients tbs_row = Coefficients::zeros(m);
        coeffs.if_set_call([&](std::size_t i) {
            tbs_row.set(m - trail.reverse_order_map()[i] - 1);
        });
        tbs.add_row(tbs_row);
    }
    return tbs;
}

RowSpace TrailBasisSpace::to_RowSpace() const {
    RowSpace rs;
    for (const Coefficients& tbs_row : *this) {
        Equation eqn = Equation::zeros(trail.span().cols());
        tbs_row.if_set_call([&](std::size_t i) {
            eqn += trail.span().row(trail.order_map()[cols()-i-1]);
        });
        rs.add_row(eqn);
    }
    return rs;
}

std::size_t TrailBasisSpace::jumpable_levels() const {
    if (rows() == 1) {
        return trail.size() - 1;
    }

    std::size_t jump = 1;
    bit::vector<> new_part;
    while (jump < trail.size()) {
        std::size_t last_decision = trail[trail.size() - 1 - jump].span.rows();
        std::size_t n_current_level = trail.span().rows() - last_decision;
        new_part = row(1).sub(n_current_level);
        if (new_part.any()) {
            return jump - 1;
        }
        ++jump;
    } 
    
    return jump - 1;
}

void TrailSnapshot::learn(const Equation& eqn, std::optional<std::reference_wrapper<Clause>> reason) {
    std::size_t inserted_index;
    span.add_row(eqn, &inserted_index);
    reasons.push_back(reason);

    // Update order map
    for (std::size_t& span_index : order_map) {
        if (span_index >= inserted_index) {
            ++span_index;
        }
    }
    order_map.push_back(inserted_index);

    // Update reverse order map
    reverse_order_map.insert(reverse_order_map.begin() + inserted_index, order_map.size() - 1);
}

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
        Equation eqn = clause.row(0);
        eqn.flip(num_vars);
        return eqn;
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
            Equation eqn = clause.row(clause.watched0);
            eqn.flip(num_vars);
            return eqn;
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

        if (dec_proc == DecisionProcedure::vsids_unit) {
            std::size_t leading_term = trail.span().row(trail.order_map()[index]).first_set();
            vsids_params.bump_activity(leading_term);
        }

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

            if (dec_proc == DecisionProcedure::vsids_unit) {
                // Bump vars in final clause
                for (std::size_t i = 0; i < asserting_clause.rows(); ++i) {
                    std::size_t var = asserting_clause.row(i).first_set();
                    vsids_params.bump_activity(var);
                }

                // Decay activity by increasing increment
                vsids_params.inc_amt *= (1 / vsids_params.decay);

                // Reset entire order_heap
                // TODO: Store a snapshot in the TrailSnapshot to avoid this
                vsids_params.order_heap = std::priority_queue<std::size_t, std::vector<std::size_t>, VsidsComparator>(VsidsComparator(vsids_params.activity));
                for (std::size_t i = 0; i < num_vars; i++) {
                    vsids_params.order_heap.push(i);
                }
            }

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
            switch (dec_proc) {
                case DecisionProcedure::fixed_unit:
                case DecisionProcedure::random_unit:
                case DecisionProcedure::random_eq: {
                    for (std::size_t i = 0; i < num_vars; ++i) {
                        Equation eqn = decision_basis[i];
                        Equation eqn_reduced = trail.span().reduce_copy(eqn);
                        Equation eqn_not_reduced = eqn_reduced;
                        eqn_not_reduced.flip(num_vars);
                        if (eqn_reduced.any() && eqn_not_reduced.any()) {
                            trail.learn(eqn, std::nullopt);
                            break;
                        }
                    }
                    break;
                }
                case DecisionProcedure::vsids_unit: {
                    Equation eqn, eqn_reduced, eqn_not_reduced;
                    do {
                        std::size_t next = vsids_params.order_heap.top();
                        vsids_params.order_heap.pop();
                        eqn = Equation::unit(num_vars + 1, next);
                        eqn_reduced = trail.span().reduce_copy(eqn);
                        eqn_not_reduced = eqn_reduced;
                        eqn_not_reduced.flip(num_vars);
                    } while (eqn_reduced.none() || eqn_not_reduced.none());
                    trail.learn(eqn, std::nullopt);
                    break;
                }
            }
        }
    }
    return ResultCase::Timeout {};
}





std::string Equation::to_string(PrintMode mode) const {
    std::ostringstream oss;
    if_set_call([&](std::size_t i) {
        if (i != size() - 1) {
            if (oss.tellp() > 0) oss << " + ";
            oss << "x" << i;
        }
    });

    auto constant = back();
    if (mode.clause_mode == ClausePrintMode::as_clause) {
        constant = !constant;
    }
    switch (mode.equation_mode) {
        case EquationPrintMode::standard:
            if (oss.tellp() == 0) oss << "0";
            oss << " = " << (constant ? "1" : "0");
            break;
        case EquationPrintMode::equals_0:
            if (oss.tellp() == 0) {
                oss << (constant ? "1" : "0") << " = 0";
            } else {
                oss << (constant ? " + 1" : "") << " = 0";
            }
            break;
        case EquationPrintMode::equals_1:
            if (oss.tellp() == 0) {
                oss << (constant ? "0" : "1") << " = 1";
            } else {
                oss << (constant ? "" : " + 1") << " = 1";
            }
            break;
    }
    
    return "[" + oss.str() + "]";
}

std::string Clause::to_string(PrintMode mode) const {
    std::ostringstream oss;
    for (const Equation& equation : *this) {
        if (oss.tellp() > 0) oss << (mode.clause_mode == ClausePrintMode::as_clause ? " v " : " ^ ");
        oss << equation.to_string(mode);
    }
    return oss.str();
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

Solver Solver::from_DIMACS(const std::string& filename, const DecisionProcedure dec_proc) {
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

