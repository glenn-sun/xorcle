#include "algebra/row_echelon_matrix.h"

#include <cassert>
#include <sstream>
#include <vector>

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

void RowSpace::compact(const CompactionTransform& transform) {
    assert(num_vars() == transform.old_num_vars() &&
           "compact: the transform was built for a different variable count");
#ifndef NDEBUG
    for (std::size_t row_idx = 0; row_idx < rows(); ++row_idx) {
        const std::size_t pivot = pivot_col_idx(row_idx);
        assert((pivot == num_vars() || transform.to_new(pivot) != CompactionTransform::npos) &&
               "compact: a pivot column is dropped");
    }
#endif
    interface().compact(transform);
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

std::vector<Equation> span_intersection(const RowEchelonMatrix& left,
                                        const RowEchelonMatrix& right) {
    assert(left.cols() == right.cols() && "span_intersection: the column counts differ");
    assert(left.repr() == right.repr() && "span_intersection: the representations differ");

    // A row of the doubled matrix whose leading column lands in the second half
    // has a zero first half, and its own leading column is the least of those of
    // the rows it is a sum of, so those rows alone make up the intersection.
    const EquationRepr repr = left.repr();
    const std::size_t cols = left.cols();

    RowEchelonMatrix doubled(repr, 2 * cols);
    std::vector<std::size_t> wide_bits;
    for (const Equation& row : left) {
        wide_bits = row.bits();
        const std::size_t half = wide_bits.size();
        for (std::size_t i = 0; i < half; ++i) {
            wide_bits.push_back(wide_bits[i] + cols);
        }
        Equation wide = Equation::with_num_bits(repr, 2 * cols);
        wide.assign_bits(wide_bits);
        doubled.add_row(std::move(wide));
    }
    for (const Equation& row : right) {
        Equation wide = Equation::with_num_bits(repr, 2 * cols);
        wide.assign_bits(row.bits());
        doubled.add_row(std::move(wide));
    }

    std::vector<Equation> basis;
    for (const Equation& row : doubled) {
        const std::optional<std::size_t> leading = row.leading_bit();
        if (!leading.has_value() || *leading < cols) {
            continue;
        }
        wide_bits = row.bits();
        for (std::size_t& bit : wide_bits) {
            bit -= cols;
        }
        Equation eqn = Equation::with_num_bits(repr, cols);
        eqn.assign_bits(wide_bits);
        basis.push_back(std::move(eqn));
    }
    return basis;
}

} // namespace xorcle
