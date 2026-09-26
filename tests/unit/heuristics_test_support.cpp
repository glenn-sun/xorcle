#include "heuristics_test_support.h"

#include "algebra_test_support.h"

#include <string>

namespace xorcle {

Clause make_clause(EquationRepr repr, std::size_t num_vars,
                   std::initializer_list<std::initializer_list<std::size_t>> rows) {
    Clause clause(repr, num_vars);
    for (const std::initializer_list<std::size_t>& row : rows) {
        clause.add_row(make_equation_with(repr, num_vars, row));
    }
    return clause;
}

RowSpace make_row_space(EquationRepr repr, std::size_t num_vars,
                        std::initializer_list<std::initializer_list<std::size_t>> rows) {
    RowSpace space(repr, num_vars);
    for (const std::initializer_list<std::size_t>& row : rows) {
        space.add_row(make_equation_with(repr, num_vars, row));
    }
    return space;
}

std::size_t dimensions_outside_span(const Clause& clause, const RowSpace& span) {
    RowSpace enlarged = span;
    std::size_t added = 0;
    for (const Equation& row : clause) {
        added += enlarged.add_row_get_row_idx(row).has_value() ? 1U : 0U;
    }
    return added;
}

std::string sign_pattern_instance(std::size_t num_vars) {
    const std::size_t num_clauses = std::size_t{1} << num_vars;
    std::string text =
        "p cnf " + std::to_string(num_vars) + " " + std::to_string(num_clauses) + "\n";
    for (std::size_t pattern = 0; pattern < num_clauses; ++pattern) {
        for (std::size_t var = 0; var < num_vars; ++var) {
            const bool negated = ((pattern >> (num_vars - 1 - var)) & 1U) != 0;
            text += (negated ? "-" : "") + std::to_string(var + 1) + " ";
        }
        text += "0\n";
    }
    return text;
}

} // namespace xorcle
