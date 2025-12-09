#include "RowSpace.h"
#include "Equation.h"

bit::vector<> RowSpace::get_unique_solution() const {
    bit::matrix<> M_copy = M;
    M_copy.to_reduced_echelon_form();
    return M_copy.col(M_copy.cols() - 1);
}

std::string Clause::to_string(PrintMode mode) const {
    std::ostringstream oss;
    for (const Equation& equation : *this) {
        if (oss.tellp() > 0) oss << (mode.clause_mode == ClausePrintMode::as_clause ? " v " : " ^ ");
        oss << equation.to_string(mode);
    }
    return oss.str();
}
