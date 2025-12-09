#pragma once

/// @brief Method to print equations.
///
/// - standard: No constants on LHS.
///
/// - equals_0: LHS = 0, possible constants on LHS.
///
/// - equals_1: LHS = 1, possible constants on LHS.
enum class EquationPrintMode { standard, equals_0, equals_1 };

/// @brief Method to print clauses.
///
/// - as_clause: OR of original equations.
///
/// - as_rowspace: AND of negated equations (as in representation).
enum class ClausePrintMode { as_clause, as_rowspace };

struct PrintMode {
    EquationPrintMode equation_mode = EquationPrintMode::standard;
    ClausePrintMode clause_mode = ClausePrintMode::as_clause;
};