#include "algebra/row_echelon_matrix.h"

#include <cassert>
#include <sstream>

namespace xorcle {

RowEchelonMatrix::RowEchelonMatrix(EquationRepr repr, std::size_t num_vars)
    : storage_(make_row_echelon_matrix_interface(repr, num_vars)) {}

RowEchelonMatrix::RowEchelonMatrix(const RowEchelonMatrix& other)
    : storage_(other.storage_->clone()) {}

RowEchelonMatrix& RowEchelonMatrix::operator=(const RowEchelonMatrix& other) {
    if (this != &other) {
        storage_ = other.storage_->clone();
    }
    return *this;
}

ColumnSet RowSpace::unique_solution() const {
    assert(rows() + 1 == cols() && "row space is not full-rank");
    assert(!pivot_columns().test(cols() - 1) && "row space contains the false equation");

    return storage().solution();
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
