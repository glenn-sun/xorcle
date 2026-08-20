#include "io/dimacs.h"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <fstream>
#include <istream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "algebra/equation.h"

namespace xorcle {

namespace {

/// @brief Header fields, once validated.
struct ParsedHeader {
    std::size_t num_vars = 0;
    std::size_t num_clauses = 0;
};

[[noreturn]] void fail(std::string_view origin, std::size_t line_number,
                       const std::string& message) {
    throw DimacsError(origin, line_number, message);
}

/// @brief Drop a trailing carriage return, so CRLF input parses like LF input.
void trim_trailing_cr(std::string& line) {
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
}

/// @brief Whether a line carries no clause or header content.
///
/// Leading whitespace is skipped before the test, so an indented `c` is still a
/// comment. This matches `tools/xnf_lib.py`.
bool is_blank_or_comment(std::string_view line) {
    const std::size_t start = line.find_first_not_of(" \t");
    return start == std::string_view::npos || line[start] == 'c';
}

/// @brief Parse a whole token as a non-negative decimal count, or `std::nullopt`.
std::optional<std::size_t> parse_count(std::string_view token) {
    std::size_t value = 0;
    const char* const first = token.data();
    const char* const last = first + token.size();
    const std::from_chars_result result = std::from_chars(first, last, value);
    if (result.ec != std::errc{} || result.ptr != last) {
        return std::nullopt;
    }
    return value;
}

/// @brief Parse a whole token as a signed decimal literal, or `std::nullopt`.
///
/// The token must be consumed in full, so `3junk` fails rather than reading `3`.
std::optional<long long> parse_signed(std::string_view token) {
    long long value = 0;
    const char* const first = token.data();
    const char* const last = first + token.size();
    const std::from_chars_result result = std::from_chars(first, last, value);
    if (result.ec != std::errc{} || result.ptr != last) {
        return std::nullopt;
    }
    return value;
}

ParsedHeader parse_header_line(const std::string& line, std::string_view origin,
                               std::size_t line_number) {
    std::istringstream stream{line};
    std::string p_token;
    std::string format_token;
    std::string num_vars_token;
    std::string num_clauses_token;
    std::string trailing_token;

    if (!(stream >> p_token >> format_token >> num_vars_token >> num_clauses_token)) {
        fail(origin, line_number,
             "header must be 'p <cnf|xnf> <num_vars> <num_clauses>', got: " + line);
    }
    if (stream >> trailing_token) {
        fail(origin, line_number, "unexpected token after the header: " + trailing_token);
    }
    if (p_token != "p") {
        fail(origin, line_number, "header must start with 'p', got: " + p_token);
    }
    if (format_token != "cnf" && format_token != "xnf") {
        fail(origin, line_number, "unsupported format token, expected 'cnf' or 'xnf', got: " +
                                      format_token);
    }

    const std::optional<std::size_t> num_vars = parse_count(num_vars_token);
    if (!num_vars) {
        fail(origin, line_number, "header variable count is not a number: " + num_vars_token);
    }
    const std::optional<std::size_t> num_clauses = parse_count(num_clauses_token);
    if (!num_clauses) {
        fail(origin, line_number, "header clause count is not a number: " + num_clauses_token);
    }
    return ParsedHeader{*num_vars, *num_clauses};
}

void push_variable(std::string_view term, std::string_view token, std::size_t num_vars,
                   std::vector<std::size_t>& bits, std::string_view origin,
                   std::size_t line_number) {
    if (term.empty()) {
        fail(origin, line_number, "empty variable in token: " + std::string{token});
    }
    const std::optional<std::size_t> variable = parse_count(term);
    if (!variable) {
        fail(origin, line_number, "variable is not a number in token: " + std::string{token});
    }
    if (*variable == 0 || *variable > num_vars) {
        fail(origin, line_number, "variable out of range in token: " + std::string{token});
    }
    bits.push_back(*variable - 1);
}

/// @brief Decode one literal token into the set bits of its stored row.
///
/// The stored row is the literal's linear negation, which is why a leading `-`
/// (right-hand side 0) sets the constant bit and a bare token does not.
std::vector<std::size_t> parse_clause_token(std::string_view token, std::size_t num_vars,
                                            std::string_view origin, std::size_t line_number) {
    std::vector<std::size_t> bits;

    if (token.find('+') == std::string_view::npos) {
        const std::optional<long long> literal = parse_signed(token);
        if (!literal) {
            fail(origin, line_number, "not a literal token: " + std::string{token});
        }
        if (*literal == 0) {
            fail(origin, line_number, "the clause terminator 0 is not a literal token");
        }
        const unsigned long long magnitude =
            *literal < 0 ? 0ull - static_cast<unsigned long long>(*literal)
                         : static_cast<unsigned long long>(*literal);
        if (magnitude > num_vars) {
            fail(origin, line_number, "variable out of range in token: " + std::string{token});
        }
        bits.push_back(static_cast<std::size_t>(magnitude) - 1);
        if (*literal < 0) {
            bits.push_back(num_vars);
        }
        return bits;
    }

    const bool rhs_is_zero = token.front() == '-';
    const std::string_view body = rhs_is_zero ? token.substr(1) : token;
    if (body.empty()) {
        fail(origin, line_number, "empty parity expression in token: " + std::string{token});
    }
    if (body.find('-') != std::string_view::npos) {
        fail(origin, line_number, "'-' inside a parity expression in token: " +
                                      std::string{token});
    }

    std::size_t term_start = 0;
    while (true) {
        const std::size_t plus_pos = body.find('+', term_start);
        const std::size_t term_length =
            plus_pos == std::string_view::npos ? body.size() - term_start : plus_pos - term_start;
        push_variable(body.substr(term_start, term_length), token, num_vars, bits, origin,
                      line_number);
        if (plus_pos == std::string_view::npos) {
            break;
        }
        term_start = plus_pos + 1;
        if (term_start == body.size()) {
            // A trailing '+' names an empty variable.
            fail(origin, line_number, "empty variable in token: " + std::string{token});
        }
    }

    if (rhs_is_zero) {
        bits.push_back(num_vars);
    }
    return bits;
}

Clause parse_clause_line(const std::string& line, std::size_t num_vars, EquationRepr repr,
                         std::string_view origin, std::size_t line_number) {
    Clause clause(repr, num_vars);
    std::istringstream stream{line};
    std::string token;
    bool terminated = false;

    while (stream >> token) {
        if (token == "0") {
            terminated = true;
            break;
        }
        const std::vector<std::size_t> bits =
            parse_clause_token(token, num_vars, origin, line_number);
        clause.add_row(Equation::from_bits(repr, num_vars, bits));
    }

    if (!terminated) {
        fail(origin, line_number, "clause line is missing its terminating 0: " + line);
    }
    if (stream >> token) {
        // Rejected rather than dropped: `tools/xnf_lib.py` reads the body as one
        // token stream and would start a second clause here.
        fail(origin, line_number, "unexpected token after the clause terminator: " + token);
    }
    return clause;
}

} // namespace

DimacsError::DimacsError(std::string_view origin, std::size_t line_number,
                         std::string_view message)
    : std::runtime_error(line_number == 0
                             ? std::string{origin} + ": " + std::string{message}
                             : std::string{origin} + ":" + std::to_string(line_number) + ": " +
                                   std::string{message}) {}

ParsedDimacs parse_dimacs(std::istream& input, EquationRepr repr, std::string_view origin) {
    std::string line;
    std::size_t line_number = 0;
    bool found_header = false;

    while (std::getline(input, line)) {
        ++line_number;
        trim_trailing_cr(line);
        if (!is_blank_or_comment(line)) {
            found_header = true;
            break;
        }
    }
    if (!found_header) {
        fail(origin, 0, "missing DIMACS/XNF header");
    }

    const ParsedHeader header = parse_header_line(line, origin, line_number);

    ParsedDimacs parsed;
    parsed.num_vars = header.num_vars;
    // Capped, because the header is untrusted: a bogus count must not turn into
    // a bogus allocation before the clause lines have been read.
    parsed.clauses.reserve(std::min<std::size_t>(header.num_clauses, 1u << 16));

    while (std::getline(input, line)) {
        ++line_number;
        trim_trailing_cr(line);
        if (is_blank_or_comment(line)) {
            continue;
        }
        parsed.clauses.push_back(
            parse_clause_line(line, header.num_vars, repr, origin, line_number));
    }

    if (parsed.clauses.size() != header.num_clauses) {
        fail(origin, 0, "clause count mismatch: header declares " +
                            std::to_string(header.num_clauses) + ", found " +
                            std::to_string(parsed.clauses.size()));
    }
    return parsed;
}

ParsedDimacs parse_dimacs_string(std::string_view text, EquationRepr repr,
                                 std::string_view origin) {
    std::istringstream stream{std::string{text}};
    return parse_dimacs(stream, repr, origin);
}

ParsedDimacs parse_dimacs_file(const std::filesystem::path& path, EquationRepr repr) {
    std::ifstream file{path};
    if (!file.is_open()) {
        throw DimacsError(path.string(), 0, "unable to open input file");
    }
    return parse_dimacs(file, repr, path.string());
}

} // namespace xorcle
