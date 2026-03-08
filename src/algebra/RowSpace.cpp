#include "algebra/RowSpace.h"
#include "algebra/Equation.h"
#include <stdexcept>
#include <sstream>

namespace algebra {
bit::vector<> RowSpace::get_unique_solution() const {
    if (rows() + 1 != cols()) {
        throw std::runtime_error("RowSpace::get_unique_solution: row space is not full-rank");
    }
    if (get_pivot_cols().element(cols() - 1)) {
        throw std::runtime_error("RowSpace::get_unique_solution: row space contains the false equation");
    }

    bit::matrix<> M_copy = M;
    M_copy.to_reduced_echelon_form();
    return M_copy.col(M_copy.cols() - 1);
}

std::string Clause::to_string(PrintMode mode) const {
    std::ostringstream oss;
    for (const auto& equation : *this) {
        if (oss.tellp() > 0) oss << (mode.clause_mode == ClausePrintMode::as_clause ? " v " : " ^ ");
        oss << Equation(equation).to_string(mode);
    }
    return oss.str();
}
} // namespace algebra
