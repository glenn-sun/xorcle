#include "io/dimacs.h"

#include <algorithm>
#include <cassert>
#include <charconv>
#include <cstddef>
#include <fstream>
#include <istream>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "algebra/equation.h"
#include "util/index_set.h"

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

/// @brief Keep each bit named an odd number of times, once, as addition does.
void cancel_repeated_bits(std::vector<std::size_t>& bits) {
    std::sort(bits.begin(), bits.end());
    auto out = bits.begin();
    for (auto it = bits.begin(); it != bits.end();) {
        auto next = std::find_if(it, bits.end(), [&](std::size_t bit) { return bit != *it; });
        if ((next - it) % 2 != 0) {
            *out++ = *it;
        }
        it = next;
    }
    bits.erase(out, bits.end());
}

/// @brief The bit of a DIMACS literal's variable.
std::size_t literal_bit(long long literal, std::string_view token, std::size_t num_vars,
                        std::string_view origin, std::size_t line_number) {
    if (literal == 0) {
        fail(origin, line_number, "the clause terminator 0 is not a literal");
    }
    const unsigned long long magnitude = literal < 0
                                             ? 0ull - static_cast<unsigned long long>(literal)
                                             : static_cast<unsigned long long>(literal);
    if (magnitude > num_vars) {
        fail(origin, line_number, "variable out of range in token: " + std::string{token});
    }
    return static_cast<std::size_t>(magnitude) - 1;
}

/// @brief Decode one equation token into the set bits of its stored row.
///
/// The stored row is the equation's linear negation, which is why a leading `-`
/// (right-hand side 0) sets the constant bit and a bare token does not.
std::vector<std::size_t> parse_clause_token(std::string_view token, std::size_t num_vars,
                                            std::string_view origin, std::size_t line_number) {
    std::vector<std::size_t> bits;

    if (!token.contains('+')) {
        const std::optional<long long> literal = parse_signed(token);
        if (!literal) {
            fail(origin, line_number, "not an equation token: " + std::string{token});
        }
        bits.push_back(literal_bit(*literal, token, num_vars, origin, line_number));
        if (*literal < 0) {
            bits.push_back(num_vars);
        }
        return bits;
    }

    const bool rhs_is_zero = token.front() == '-';
    const std::string_view body = rhs_is_zero ? token.substr(1) : token;
    assert(!body.empty() && "a token holding '+' left nothing after its leading '-'");
    if (body.contains('-')) {
        fail(origin, line_number, "'-' inside an XOR expression in token: " +
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
            fail(origin, line_number, "empty variable in token: " + std::string{token});
        }
    }

    cancel_repeated_bits(bits);
    if (rhs_is_zero) {
        bits.push_back(num_vars);
    }
    return bits;
}

/// @brief The clause lines' equations as set bits over the declared variables.
struct ClauseLines {
    std::vector<std::size_t> bits;          ///< Every equation's set bits, concatenated.
    std::vector<std::size_t> equation_ends; ///< Into `bits`.
    std::vector<std::size_t> clause_ends;   ///< Into `equation_ends`.
    std::size_t num_xor_lines = 0;

    void end_equation(const std::vector<std::size_t>& equation_bits) {
        bits.insert(bits.end(), equation_bits.begin(), equation_bits.end());
        equation_ends.push_back(bits.size());
    }

    void end_clause() { clause_ends.push_back(equation_ends.size()); }
};

void parse_clause_line(const std::string& line, std::size_t num_vars, ClauseLines& lines,
                       std::string_view origin, std::size_t line_number) {
    std::istringstream stream{line};
    std::string token;
    bool terminated = false;

    while (stream >> token) {
        if (token == "0") {
            terminated = true;
            break;
        }
        lines.end_equation(parse_clause_token(token, num_vars, origin, line_number));
    }

    if (!terminated) {
        fail(origin, line_number, "clause line is missing its terminating 0: " + line);
    }
    if (stream >> token) {
        fail(origin, line_number, "unexpected token after the clause terminator: " + token);
    }
    lines.end_clause();
}

/// @brief Parse a CNF-XOR line, its leading `x` removed, into a clause of one equation.
///
/// The literals XOR to 1, so each negated literal flips the constant, and `x 0` is
/// `0 = 1`. The stored row is the linear negation, whose constant bit is the parity
/// of the negated literals.
void parse_xor_line(std::string_view body, const std::string& line, std::size_t num_vars,
                    ClauseLines& lines, std::string_view origin, std::size_t line_number) {
    std::istringstream stream{std::string{body}};
    std::string token;
    std::vector<std::size_t> bits;
    bool odd_negations = false;
    bool terminated = false;

    while (stream >> token) {
        if (token == "0") {
            terminated = true;
            break;
        }
        const std::optional<long long> literal = parse_signed(token);
        if (!literal) {
            fail(origin, line_number, "not a literal in an XOR line: " + token);
        }
        bits.push_back(literal_bit(*literal, token, num_vars, origin, line_number));
        odd_negations ^= *literal < 0;
    }

    if (!terminated) {
        fail(origin, line_number, "XOR line is missing its terminating 0: " + line);
    }
    if (stream >> token) {
        fail(origin, line_number, "unexpected token after the XOR line's terminator: " + token);
    }

    cancel_repeated_bits(bits);
    if (odd_negations) {
        bits.push_back(num_vars);
    }
    lines.end_equation(bits);
    lines.end_clause();
    ++lines.num_xor_lines;
}

/// @brief The variables some equation of `lines` uses, ascending.
std::vector<std::size_t> used_vars(const ClauseLines& lines, std::size_t num_vars) {
    IndexSet used(num_vars + 1);
    for (const std::size_t bit : lines.bits) {
        used.set(bit);
    }
    used.reset(num_vars);
    std::vector<std::size_t> vars;
    vars.reserve(used.count());
    used.for_each_set([&](std::size_t var) { vars.push_back(var); });
    return vars;
}

/// @brief Build the clauses over the used variables alone, renumbered in ascending order.
std::vector<Clause> build_clauses(ClauseLines lines, std::size_t num_vars,
                                  const std::vector<std::size_t>& used_vars,
                                  EquationRepr repr) {
    const std::size_t num_used_vars = used_vars.size();
    if (num_used_vars != num_vars) {
        std::vector<std::size_t> bit_to_used_bit(num_vars + 1);
        for (std::size_t used_bit = 0; used_bit < num_used_vars; ++used_bit) {
            bit_to_used_bit[used_vars[used_bit]] = used_bit;
        }
        bit_to_used_bit[num_vars] = num_used_vars;
        for (std::size_t& bit : lines.bits) {
            bit = bit_to_used_bit[bit];
        }
    }

    std::vector<Clause> clauses;
    clauses.reserve(lines.clause_ends.size());
    const std::span<const std::size_t> bits{lines.bits};
    std::size_t equation_idx = 0;
    std::size_t bit_idx = 0;
    for (const std::size_t clause_end : lines.clause_ends) {
        Clause clause(repr, num_used_vars);
        for (; equation_idx < clause_end; ++equation_idx) {
            const std::size_t equation_end = lines.equation_ends[equation_idx];
            clause.add_row(Equation::from_bits(repr, num_used_vars,
                                               bits.subspan(bit_idx, equation_end - bit_idx)));
            bit_idx = equation_end;
        }
        clauses.push_back(std::move(clause));
    }
    return clauses;
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

    ClauseLines lines;
    while (std::getline(input, line)) {
        ++line_number;
        trim_trailing_cr(line);
        if (is_blank_or_comment(line)) {
            continue;
        }
        const std::size_t start = line.find_first_not_of(" \t");
        if (line[start] == 'x') {
            parse_xor_line(std::string_view{line}.substr(start + 1), line, header.num_vars, lines,
                           origin, line_number);
        } else {
            parse_clause_line(line, header.num_vars, lines, origin, line_number);
        }
    }

    const std::size_t num_clauses = lines.clause_ends.size();
    const std::size_t num_xor_lines = lines.num_xor_lines;
    if (header.num_clauses != num_clauses && header.num_clauses != num_clauses - num_xor_lines) {
        std::string message = "clause count mismatch: header declares " +
                              std::to_string(header.num_clauses) + ", found " +
                              std::to_string(num_clauses);
        if (num_xor_lines != 0) {
            message += ", including " + std::to_string(num_xor_lines) + " XOR lines";
        }
        fail(origin, 0, message);
    }

    ParsedDimacs parsed;
    parsed.num_vars = header.num_vars;
    parsed.used_vars = used_vars(lines, header.num_vars);
    parsed.clauses = build_clauses(std::move(lines), header.num_vars, parsed.used_vars, repr);
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
