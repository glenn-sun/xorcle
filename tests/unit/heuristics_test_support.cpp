#include "heuristics_test_support.h"

#include "algebra_test_support.h"

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

} // namespace xorcle
