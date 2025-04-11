#include <fstream>
#include <sstream>
#include "Solver.h"

std::string to_string(const Equation& eqn, const PrintMode mode) {
    std::ostringstream oss;
    for (std::size_t i = 1; i < eqn.size(); ++i) {
        if (eqn[i]) {
            if (oss.tellp() > 0) oss << " + ";
            oss << "x" << i;
        }
    }
    switch (mode) {
        case PrintMode::standard:
            if (oss.tellp() == 0) oss << "0";
            oss << " = " << (eqn[0] ? "1" : "0");
            break;
        case PrintMode::equals_0:
            if (oss.tellp() == 0) {
                oss << (eqn[0] ? "1" : "0") << " = 0";
            } else {
                oss << (eqn[0] ? " + 1" : "") << " = 0";
            }
            break;
        case PrintMode::equals_1:
            if (oss.tellp() == 0) {
                oss << (eqn[0] ? "0" : "1") << " = 1";
            } else {
                oss << (eqn[0] ? "" : " + 1") << " = 1";
            }
            break;
    }
    
    return "[" + oss.str() + "]";
}

std::string to_string(const Clause& c, const PrintMode mode) {
    std::ostringstream oss;
    for (const auto& eqn : c) {
        if (oss.tellp() > 0) oss << " v ";
        oss << to_string(eqn, mode);
    }
    return oss.str();
}

void Solver::print(const PrintMode mode) const {
    std::cout << "Solver with " << num_vars << " variables, " << clauses.size() << " clauses:\n";
        for (const auto& clause : clauses) {
            std::cout << to_string(clause, mode) << '\n';
        }
}

Solver from_DIMACS(const std::string& filename) {
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
                    if (literal < 0) {
                        is_neg = !is_neg;
                        literal = std::abs(literal);
                    }
                    equation.set(literal);
                }
                if (!is_neg) {
                    equation.set(0);
                }
            } else { // Process legacy DIMACs-type term
                int literal{std::stoi(token)};
                if (literal < 0) {
                    literal = std::abs(literal);
                } else {
                    equation.set(0);
                }
                equation.set(literal);
            }

            clause.push_back(equation);
        }
        solver.add_clause(clause);
    }
    file.close();
    return solver;
}

