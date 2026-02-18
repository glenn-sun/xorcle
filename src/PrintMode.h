#pragma once

/// @brief Method to print equations.
enum class EquationPrintMode {
    standard, ///< No constants on LHS.
    equals_0, ///< LHS = 0, possible constants on LHS.
    equals_1  ///< LHS = 1, possible constants on LHS.
};

/// @brief Method to print clauses.
enum class ClausePrintMode {
    as_clause,   ///< OR of original equations.
    as_rowspace  ///< AND of negated equations (as in representation).
};

/// @brief Combined print-mode configuration for equations and clauses.
struct PrintMode {
    EquationPrintMode equation_mode = EquationPrintMode::standard; ///< Equation formatting.
    ClausePrintMode clause_mode = ClausePrintMode::as_clause;      ///< Clause formatting.
};
