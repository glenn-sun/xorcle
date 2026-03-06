#include "io/DimacsParser.h"

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <cstdlib>
#include <utility>

#include "algebra/Equation.h"

namespace io {
ParsedDimacs parse_dimacs_file(const std::string& filename) {
    std::ifstream file{filename};
    std::string line;

    if (!file.is_open()) {
        throw std::runtime_error("Unable to open file: " + filename);
    }

    // Skip comments and empty lines until header
    while (std::getline(file, line) && (line.empty() || line[0] == 'c')) {}

    if (line.empty() || line[0] != 'p') {
        throw std::runtime_error("Invalid DIMACS header: " + line);
    }

    std::istringstream header_ss{line};
    std::string p_tok, cnf_tok;
    std::size_t num_vars;
    std::size_t tmp_num_clauses;
    header_ss >> p_tok >> cnf_tok >> num_vars >> tmp_num_clauses;

    ParsedDimacs parsed;
    parsed.num_vars = num_vars;
    parsed.clauses.reserve(tmp_num_clauses);

    // Read clause lines
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == 'c') continue;

        // remove any trailing CR (windows files)
        if (!line.empty() && line.back() == '\r') line.pop_back();

        algebra::Clause clause(num_vars);
        std::istringstream iss{line};
        std::string token;

        while (iss >> token) {
            if (token == "0") break;

            algebra::Equation equation{algebra::Equation::zeros(num_vars + 1)};

            // If token contains '+' it's a plus-expression (equation)
            if (token.find('+') != std::string::npos) {
                bool leading_neg = false;
                size_t pos = 0;

                // allow a leading '-' only; disallow '-' elsewhere
                if (!token.empty() && token[0] == '-') {
                    leading_neg = true;
                    pos = 1; // skip the leading '-'
                }

                // ensure there is no '-' anywhere else in the token
                if (token.find('-', pos) != std::string::npos) {
                    throw std::runtime_error("Illegal '-' inside XOR expression: " + token);
                }

                // substring to split
                std::string body = token.substr(pos);
                if (body.empty()) {
                    throw std::runtime_error("Empty XOR expression after '-' in token: " + token);
                }

                // split on '+'
                size_t start = 0;
                while (start < body.size()) {
                    size_t plus_pos = body.find('+', start);
                    std::string var_str = (plus_pos == std::string::npos)
                                          ? body.substr(start)
                                          : body.substr(start, plus_pos - start);

                    if (var_str.empty()) {
                        throw std::runtime_error("Empty variable in XOR expression: " + token);
                    }

                    int var;
                    try {
                        var = std::stoi(var_str);
                    } catch (...) {
                        throw std::runtime_error("Invalid integer in XOR expression: " + var_str);
                    }

                    if (var <= 0 || static_cast<std::size_t>(var) > num_vars) {
                        throw std::runtime_error("Variable out of range: " + var_str);
                    }

                    equation.set(var - 1);

                    if (plus_pos == std::string::npos) break;
                    start = plus_pos + 1;
                }

                if (leading_neg) {
                    // leading negative toggles parity / RHS -> same as other format
                    equation.set(num_vars);
                }

            } else { // legacy single literal (possibly negative)
                int literal;
                try {
                    literal = std::stoi(token);
                } catch (...) {
                    throw std::runtime_error("Invalid literal token: " + token);
                }

                if (literal == 0) {
                    break; // defensive: if someone wrote "0" without whitespace
                }

                if (literal < 0) {
                    literal = std::abs(literal);
                    equation.set(num_vars); // toggle parity for negative single-literal token
                }

                if (literal <= 0 || static_cast<std::size_t>(literal) > num_vars) {
                    throw std::runtime_error("Variable out of range: " + token);
                }

                equation.set(literal - 1);
            }

            clause.add_row(equation);
        }

        parsed.clauses.push_back(std::move(clause));
    }

    file.close();
    return parsed;
}
} // namespace io
