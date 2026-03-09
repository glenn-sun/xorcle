#include "io/DimacsParser.h"

#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

#include "algebra/Equation.h"

namespace io {

namespace {

struct ParsedHeader {
    std::size_t num_vars = 0;
    std::size_t num_clauses = 0;
};

void trim_trailing_cr(std::string& line) {
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
}

bool is_comment_or_empty(const std::string& line) {
    return line.empty() || line[0] == 'c';
}

ParsedHeader parse_header_line(const std::string& header_line) {
    std::istringstream header_stream{header_line};
    std::string p_token;
    std::string format_token;
    ParsedHeader header;
    std::string trailing_token;

    if (!(header_stream >> p_token >> format_token >> header.num_vars >> header.num_clauses) ||
        (header_stream >> trailing_token)) {
        throw std::runtime_error("Invalid DIMACS/XNF header: " + header_line);
    }
    if (p_token != "p") {
        throw std::runtime_error("Invalid DIMACS/XNF header prefix: " + header_line);
    }
    if (format_token != "cnf" && format_token != "xnf") {
        throw std::runtime_error("Unsupported DIMACS/XNF format token: " + format_token);
    }
    return header;
}

algebra::Equation parse_clause_token(const std::string& token, std::size_t num_vars) {
    algebra::Equation equation{algebra::Equation::zeros(num_vars + 1)};

    if (token.find('+') == std::string::npos) {
        int literal = 0;
        try {
            literal = std::stoi(token);
        } catch (...) {
            throw std::runtime_error("Invalid literal token: " + token);
        }

        if (literal == 0) {
            throw std::runtime_error("Clause terminator must not be parsed as a literal token");
        }
        if (literal < 0) {
            literal = std::abs(literal);
            equation.set(num_vars);
        }
        if (literal <= 0 || static_cast<std::size_t>(literal) > num_vars) {
            throw std::runtime_error("Variable out of range: " + token);
        }

        equation.set(static_cast<std::size_t>(literal - 1));
        return equation;
    }

    bool leading_negation = false;
    std::size_t token_pos = 0;
    if (!token.empty() && token[0] == '-') {
        leading_negation = true;
        token_pos = 1;
    }

    if (token.find('-', token_pos) != std::string::npos) {
        throw std::runtime_error("Illegal '-' inside XOR expression: " + token);
    }

    const std::string xor_body = token.substr(token_pos);
    if (xor_body.empty()) {
        throw std::runtime_error("Empty XOR expression after '-' in token: " + token);
    }

    std::size_t term_start = 0;
    while (term_start < xor_body.size()) {
        const std::size_t plus_pos = xor_body.find('+', term_start);
        const std::string variable_token =
            (plus_pos == std::string::npos)
                ? xor_body.substr(term_start)
                : xor_body.substr(term_start, plus_pos - term_start);

        if (variable_token.empty()) {
            throw std::runtime_error("Empty variable in XOR expression: " + token);
        }

        int variable = 0;
        try {
            variable = std::stoi(variable_token);
        } catch (...) {
            throw std::runtime_error("Invalid integer in XOR expression: " + variable_token);
        }
        if (variable <= 0 || static_cast<std::size_t>(variable) > num_vars) {
            throw std::runtime_error("Variable out of range: " + variable_token);
        }

        equation.set(static_cast<std::size_t>(variable - 1));

        if (plus_pos == std::string::npos) {
            break;
        }
        term_start = plus_pos + 1;
    }

    if (leading_negation) {
        equation.set(num_vars);
    }

    return equation;
}

algebra::Clause parse_clause_line(const std::string& line, std::size_t num_vars) {
    algebra::Clause clause(num_vars);
    std::istringstream line_stream{line};
    std::string token;
    bool found_terminator = false;

    while (line_stream >> token) {
        if (token == "0") {
            found_terminator = true;
            break;
        }
        clause.add_row(parse_clause_token(token, num_vars));
    }

    if (!found_terminator) {
        throw std::runtime_error("Clause line missing terminating 0: " + line);
    }

    return clause;
}

} // namespace

ParsedDimacs parse_dimacs_file(const std::string& filename) {
    std::ifstream file{filename};
    std::string line;

    if (!file.is_open()) {
        throw std::runtime_error("Unable to open file: " + filename);
    }

    while (std::getline(file, line)) {
        trim_trailing_cr(line);
        if (!is_comment_or_empty(line)) {
            break;
        }
    }

    if (line.empty()) {
        throw std::runtime_error("Missing DIMACS/XNF header in file: " + filename);
    }

    const ParsedHeader header = parse_header_line(line);

    ParsedDimacs parsed;
    parsed.num_vars = header.num_vars;
    parsed.clauses.reserve(header.num_clauses);

    while (std::getline(file, line)) {
        trim_trailing_cr(line);
        if (is_comment_or_empty(line)) {
            continue;
        }
        parsed.clauses.push_back(parse_clause_line(line, header.num_vars));
    }

    if (parsed.clauses.size() != header.num_clauses) {
        throw std::runtime_error("Clause count mismatch: header declares " +
                                 std::to_string(header.num_clauses) + ", parsed " +
                                 std::to_string(parsed.clauses.size()));
    }

    return parsed;
}
} // namespace io
