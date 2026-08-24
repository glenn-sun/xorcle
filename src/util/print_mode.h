#pragma once

namespace xorcle {

enum class EquationPrintMode {
    standard, ///< No constants on LHS.
    equals_0, ///< LHS = 0, possible constants on LHS.
    equals_1  ///< LHS = 1, possible constants on LHS.
};

enum class ClausePrintMode {
    as_clause,          ///< OR of original equations.
    as_linear_negation  ///< AND of negated equations, as stored.
};

struct PrintMode {
    EquationPrintMode equation_mode = EquationPrintMode::standard;
    ClausePrintMode clause_mode = ClausePrintMode::as_clause;
};

} // namespace xorcle
