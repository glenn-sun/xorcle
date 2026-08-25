#include "algebra/row_echelon_matrix.h"

#include <cassert>
#include <sstream>

namespace xorcle {

RowEchelonMatrix::RowEchelonMatrix(EquationRepr repr, std::size_t cols)
    : interface_(make_row_echelon_matrix_interface(repr, cols)) {}

RowEchelonMatrix::RowEchelonMatrix(const RowEchelonMatrix& other)
    : interface_(other.interface_->clone()) {}

RowEchelonMatrix& RowEchelonMatrix::operator=(const RowEchelonMatrix& other) {
    if (this != &other) {
        interface_ = other.interface_->clone();
    }
    return *this;
}

IndexSet RowSpace::unique_solution() const {
    assert(rows() + 1 == cols() && "row space is not full-rank");
    assert(!pivot_col_idxs().test(cols() - 1) && "row space contains the false equation");

    return interface().solution();
}

std::string Clause::to_string(const PrintMode& mode) const {
    std::ostringstream oss;
    for (const Equation& equation : *this) {
        if (oss.tellp() > 0) {
            oss << (mode.clause_mode == ClausePrintMode::as_clause ? " v " : " ^ ");
        }
        oss << equation.to_string(mode);
    }
    return oss.str();
}

} // namespace xorcle
