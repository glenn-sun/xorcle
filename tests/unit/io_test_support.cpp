#include "io_test_support.h"

#include <algorithm>
#include <iterator>
#include <sstream>
#include <utility>

#include <unistd.h>

#include "io/proof_writer.h"

namespace xorcle {

std::filesystem::path scratch_path(std::string_view name) {
    return std::filesystem::temp_directory_path() /
           ("xorcle_test_" + std::to_string(::getpid()) + "_" + std::string{name});
}

std::vector<std::vector<ProofRow>> clause_rows(const std::vector<Clause>& clauses) {
    std::vector<std::vector<ProofRow>> rows;
    rows.reserve(clauses.size());
    for (const Clause& clause : clauses) {
        std::vector<ProofRow>& out = rows.emplace_back();
        for (const Equation& row : clause) {
            out.push_back(row.bits());
        }
    }
    return rows;
}

std::vector<std::vector<ProofRow>> clause_rows(const ParsedDimacs& parsed) {
    std::vector<std::vector<ProofRow>> rows;
    rows.reserve(parsed.clauses.size());
    for (const Clause& clause : parsed.clauses) {
        std::vector<ProofRow>& out = rows.emplace_back();
        for (const Equation& row : clause) {
            out.push_back(lift_row(row, parsed.used_vars, parsed.num_vars));
        }
    }
    return rows;
}

LrupChecker::LrupChecker(EquationRepr repr, std::size_t num_vars,
                         const std::vector<std::vector<ProofRow>>& inputs)
    : repr_(repr), num_vars_(num_vars), max_id_(inputs.size()) {
    for (std::size_t index = 0; index < inputs.size(); ++index) {
        clauses_.emplace(index + 1, inputs[index]);
    }
}

std::string LrupChecker::check(const std::string& text) {
    std::istringstream text_stream(text);
    std::string line;
    while (std::getline(text_stream, line)) {
        std::vector<std::string> tokens;
        std::istringstream line_stream(line);
        for (std::string token; line_stream >> token;) {
            tokens.push_back(token);
        }
        if (tokens.empty() || tokens[0].starts_with('c')) {
            continue;
        }
        std::string error = check_line(tokens);
        if (!error.empty()) {
            error.append(" in '").append(line).append("'");
            return error;
        }
    }
    return refuted_ ? "" : "no line derives the empty clause";
}

std::string LrupChecker::check_line(const std::vector<std::string>& tokens) {
    if (tokens.back() != "0") {
        return "a proof line must end with 0";
    }
    const auto zeros = std::count(tokens.begin(), tokens.end(), "0");
    if (zeros == 1) {
        if (tokens.size() == 1) {
            return "a deletion line lists no clause id";
        }
        for (std::size_t i = 0; i + 1 < tokens.size(); ++i) {
            if (clauses_.erase(std::stoul(tokens[i])) == 0) {
                return "deletion of clause id " + tokens[i] + ", which does not exist";
            }
        }
        return "";
    }
    if (zeros != 2) {
        return "a proof line must contain exactly one or two zeros";
    }
    const auto first = std::find(tokens.begin(), tokens.end(), "0");
    if (first == tokens.begin()) {
        return "missing learned clause id";
    }

    const std::size_t new_id = std::stoul(tokens[0]);
    if (new_id <= max_id_) {
        return "learned id " + tokens[0] + " does not exceed every earlier id";
    }
    Rows rows;
    for (auto token = tokens.begin() + 1; token != first; ++token) {
        rows.push_back(parse_token(*token));
    }

    RowSpace basis(repr_, num_vars_);
    for (const ProofRow& clause_row : rows) {
        basis.add_row(equation(clause_row));
    }
    bool verified = false;
    for (auto token = std::next(first); token + 1 != tokens.end(); ++token) {
        const auto reason = clauses_.find(std::stoul(*token));
        if (reason == clauses_.end()) {
            return "missing reason clause " + *token;
        }
        const Status status = propagate(reason->second, basis);
        if (status == Status::conflict) {
            verified = true;
            break;
        }
        if (status == Status::none) {
            return "reason clause " + *token + " did not propagate";
        }
    }
    if (!verified) {
        return "no conflict derived for " + tokens[0];
    }
    refuted_ = refuted_ || rows.empty();
    clauses_[new_id] = std::move(rows);
    max_id_ = new_id;
    return "";
}

LrupChecker::Status LrupChecker::propagate(const Rows& rows, RowSpace& basis) const {
    std::vector<ProofRow> residues;
    for (const ProofRow& clause_row : rows) {
        Equation reduced = equation(clause_row);
        basis.reduce(reduced);
        if (reduced.leading_bit()) {
            residues.push_back(reduced.bits());
        }
    }
    if (residues.empty()) {
        return Status::conflict;
    }
    for (const ProofRow& residue : residues) {
        if (residue != residues[0]) {
            return Status::none;
        }
    }
    const ProofRow deduced = negate(residues[0]);
    if (contains(basis, deduced)) {
        return Status::already_known;
    }
    basis.add_row(equation(deduced));
    return Status::deduced;
}

bool LrupChecker::contains(const RowSpace& basis, const ProofRow& bits) const {
    Equation reduced = equation(bits);
    basis.reduce(reduced);
    return !reduced.leading_bit().has_value();
}

Equation LrupChecker::equation(const ProofRow& bits) const {
    Equation out = make_equation(repr_, num_vars_);
    out.assign_bits(bits);
    return out;
}

ProofRow LrupChecker::negate(const ProofRow& bits) const {
    ProofRow out = bits;
    if (!out.empty() && out.back() == num_vars_) {
        out.pop_back();
    } else {
        out.push_back(num_vars_);
    }
    return out;
}

ProofRow LrupChecker::parse_token(const std::string& token) const {
    ProofRow bits;
    std::size_t position = 0;
    const bool constant = !token.empty() && token[0] == '-';
    if (constant) {
        position = 1;
    }
    while (position < token.size()) {
        const std::size_t plus = token.find('+', position);
        const std::size_t length = plus == std::string::npos ? plus : plus - position;
        bits.push_back(std::stoul(token.substr(position, length)) - 1);
        if (plus == std::string::npos) {
            break;
        }
        position = plus + 1;
    }
    std::sort(bits.begin(), bits.end());
    if (constant) {
        bits.push_back(num_vars_);
    }
    return bits;
}

} // namespace xorcle
