#include "algebra/row_echelon_matrix.h"

#include <cassert>
#include <sstream>

#include "algebra/backend.h"

namespace xorcle {

RowEchelonMatrix::RowEchelonMatrix(Algebra backend, std::size_t num_vars)
    : storage_(make_matrix_storage(backend, num_vars)) {}

RowEchelonMatrix::RowEchelonMatrix(const RowEchelonMatrix& other)
    : storage_(other.storage_->clone()) {}

RowEchelonMatrix& RowEchelonMatrix::operator=(const RowEchelonMatrix& other) {
    if (this != &other) {
        storage_ = other.storage_->clone();
    }
    return *this;
}

Bitmap RowSpace::unique_solution() const {
    assert(rows() + 1 == cols() && "row space is not full-rank");
    assert(!pivot_columns().test(cols() - 1) && "row space contains the false equation");

    Bitmap out(num_vars());
    storage().solution(out);
    return out;
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
