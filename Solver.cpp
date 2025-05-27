#include <fstream>
#include <sstream>
#include "Solver.h"

TrailBasisSpace TrailBasisSpace::from_RowSpace(const RowSpace& rs, const Trail& trail) {
    TrailBasisSpace tbs;
    for (const Equation& eqn : rs) {
        Equation eqn_copy = eqn;
        Equation tbs_row = Equation::zeros(trail.span().rows());
        while (!eqn_copy.none()) {
            std::size_t first_set = eqn_copy.first_set();
            
        }
    }
}

bool Clause::is_asserting(const Trail& trail) const {
    std::size_t prev = trail.size() - 2;

    // Find one equation that is non-zero
    Equation asserting_eqn;
    bool found = false;
    for (std::size_t i = 0; i < rows(); ++i) {
        asserting_eqn = trail[prev].span.reduce_copy(row(i));
        if (!asserting_eqn.none()) {
            found = true;
            break;
        }
    }
    if (!found) return false;

    // Check if all other equations are zero
    for (std::size_t i = 0; i < rows(); ++i) {
        Equation eqn = trail[prev].span.reduce_with_copy(row(i), asserting_eqn);
        if (!eqn.none()) return false;
    }
    return true;
}

void TrailSnapshot::learn(const Equation& eqn, std::optional<std::reference_wrapper<Clause>> reason) {
    std::size_t inserted_index;
    span.push_to_echelon_form(eqn, &inserted_index);
    reasons.push_back(reason);

    // Update order map
    for (std::size_t& span_index : order_map) {
        if (span_index >= inserted_index) {
            ++span_index;
        }
    }
    order_map.push_back(inserted_index);
}

void Solver::add_clause(Clause& clause) {
    if (clause.rows() == 0) { 
        return;
    }
    clauses.push_back(clause);
}


std::optional<std::pair<Equation, std::reference_wrapper<Clause>>>Solver::unit_propagate_once() {
    for (auto& clause : clauses) {
        // If clause is satisfied, don't do anything
        Equation witness = trail.span().reduce_copy(clause.row(clause.true_witness));
        witness.flip(num_vars);
        if (witness.none()) {
            continue;
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
                continue;
            }
        }

        // If clause is unit, conflict/propagate are the same
        if (clause.rows() == 1) {
            Equation eqn = clause.row(0);
            eqn.flip(num_vars);
            return std::make_pair(eqn, std::ref(clause));
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
                    found = true;
                    break;
                }
            }
            // If none found, return conflict
            if (!found) {
                return std::make_pair(eqn_false, std::ref(clause));
            }
        }
        // Postcondition: clause.row(clause.watched0) is non-zero w.r.t. current trail

        // Do the same things with clause.row(clause.watched1), but reduce using clause.row(clause.watched0).
        Equation reduced1 = trail.span().reduce_with_copy(clause.row(clause.watched1), clause.row(clause.watched0));
        if (reduced1.none()) {
            // Find a new equation to watch
            bool found = false;
            for (std::size_t i = 0; i < clause.rows(); ++i) {
                if (i == clause.watched0 || i == clause.watched1) continue;
                Equation reduced = trail.span().reduce_with_copy(clause.row(i), clause.row(clause.watched0));
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
                return std::make_pair(eqn, std::ref(clause));
            }
        }
    }
    // No deductions possible
    return std::nullopt;
}


std::optional<bit::vector<>> Solver::solve() {
    while (true) {
        // Run unit propagation
        bool conflict = false;
        while (!conflict) {
            auto propagate_result = unit_propagate_once();
            if (propagate_result) {
                auto [eqn, reason] = *propagate_result;
                trail.learn(eqn, reason);
                conflict = trail.span().span_contains(eqn_false);
            } else {
                break;
            }
        }

        // Check for conflict
        if (trail.span().span_contains(eqn_false)) {
            if (level() == 0) {
                return std::nullopt; // conflict
            }
            
            // Fact: last_reason should never be std::nullopt here
            std::optional<std::reference_wrapper<Clause>> last_reason = trail.reasons()[trail.span().rows() - 1];
            Clause conflict = last_reason->get();


            while (!conflict.is_asserting(trail)) {
                
            }
        } else {
            // Check for satisfiability
            if (trail.span().rows() == num_vars) {
                RowSpace rowspace = trail.span();
                rowspace.to_reduced_echelon_form();
                return rowspace.col(rowspace.cols() - 1);
            }

            // Make a decision
            
            // For now, decide the first unset variable.
            // TODO: Change this.
            for (std::size_t i = 0; i < num_vars; ++i) {
                Equation x = Equation::unit(num_vars + 1, i);
                Equation x_not = Equation::unit(num_vars + 1, i).flip(num_vars);
                if (!trail.span().span_contains(x) && !trail.span().span_contains(x_not)) {
                    trail.learn(x, std::nullopt);
                    break;
                }
            }
        }
    }
}

std::string to_string(const Equation& eqn, const PrintMode mode) {
    std::ostringstream oss;
    for (std::size_t i = 0; i < eqn.size() - 1; ++i) {
        if (eqn[i]) {
            if (oss.tellp() > 0) oss << " + ";
            oss << "x" << i;
        }
    }
    auto constant = eqn.back();
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

std::string to_string(const RowSpace& clause, const PrintMode mode) {
    std::ostringstream oss;
    for (const auto& equation : clause) {
        if (oss.tellp() > 0) oss << (mode.clause_mode == ClausePrintMode::as_clause ? " v " : " ^ ");
        oss << to_string(equation, mode);
    }
    return oss.str();
}

void Solver::print_clauses(const PrintMode mode) const {
    std::cout << "Solver with " << num_vars << " variables, " << clauses.size() << " clauses:" << std::endl;
    for (const auto& clause : clauses) {
        std::cout << to_string(clause, mode) << std::endl;
    }
}

void Solver::print_trail(const PrintMode mode) const {
    std::cout << "Trail of length " << trail.span().rows() << ":" << std::endl;
    std::cout << "Final row space " << std::endl << to_string(trail.span(), mode) << std::endl;

    for (std::size_t i = 0; i < trail.span().rows(); ++i) {
        std::cout << "Learning " << to_string(trail.span().row(trail.order_map()[i]), mode) << std::endl;
        if (trail.reasons()[i]) {
            std::cout << "Reason: " << std::endl << to_string(*trail.reasons()[i], mode) << std::endl;
        } else {
            std::cout << "Reason: decision" << std::endl;
        }
    }
}

Solver Solver::from_DIMACS(const std::string& filename) {
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

    Solver solver{num_vars};

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

            clause.push_to_echelon_form(equation);
        }
        solver.add_clause(clause);
    }
    file.close();
    return solver;
}

