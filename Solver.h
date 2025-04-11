#pragma once

#include <vector>
#include <string>
#include "bit/vector.h"
#include "bit/matrix.h"

/// @brief Method to print equations.
///
/// - standard: No constants on LHS.
/// - equals_0: LHS = 0, possible constants on LHS.
/// - equals_1: LHS = 1, possible constants on LHS.
enum class PrintMode { standard, equals_0, equals_1 };

/// @brief A linear equation over GF(2) equal to 0.
///
/// Represented as a bit::vector whose first component is the constant 
/// term. In other words, if there are 3 variables, the equation 
/// 1 + x1 + x3 = 0 is represented as [1, 1, 0, 1]. Note that the false
/// equation 1 = 0 has a representation in this form - when there are
/// 3 variables, it is [1, 0, 0, 0].
using Equation = bit::vector<>;

/// @brief Disjunction of linear equations, represented as a matrix
/// in row-major form.
using Clause = bit::matrix<>;


/// @brief Main solver class.
class Solver {
private:
    const std::size_t num_vars;
    std::vector<Clause> clauses;

public:
    Solver(const std::size_t num_vars) : num_vars(num_vars) {};

    inline void add_clause(const Clause& c) {
        clauses.push_back(c);
    }

    const Equation eqn_true{num_vars + 1};
    const Equation eqn_false{Equation::unit(num_vars + 1, 0)};

    void print(const PrintMode mode = PrintMode::standard) const;
};

/// @brief Create a Solver from a DIMACS file.
/// @param filename Path to a DIMACS file.
/// 
/// The DIMACS file format is extended as follows:
///
/// Instead of a list of positive and negative literals, we have
/// a list of tuples. Each tuple represents a linear equation. 
/// Variables in the equation may be positive or negative. An even 
/// number of negative literals expresses that the equation sums to 
/// 1. An odd number of negative literals expresses that the equation
/// sums to 0.
/// 
/// Spaces around parentheses are optional. As usual, each line
/// is terminated by 0. Comments are lines starting with "c". The line
/// "p cnf n m" must come before all clauses, where "n" is the number
/// of variables and "m" is the number of clauses. For backwards 
/// compatability with standard DIMACS format, "k" is treated 
/// identically to "(k)" in all clauses. 
///
/// Example: The following is a valid file that consists of four
/// equivalent ways to express (x1 + x2 = 1) OR (x3 = 0).
///
/// c This is a comment
/// p cnf 3 4
/// c This is another comment
/// (1 2) (-3) 0
/// (-1 -2) (-3) 0
/// c This is yet another comment
/// ( 1 2 ) -3 0
/// (-1 -2 ) ( -3) 0
Solver from_DIMACS(const std::string& filename);