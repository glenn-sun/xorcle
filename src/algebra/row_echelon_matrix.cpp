#include "algebra/row_echelon_matrix.h"

#include <algorithm>
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
        const std::size_t leading_col_idx = pivot_col_idx(row_idx);
        assert((leading_col_idx == num_vars() ||
                transform.to_new(leading_col_idx) != CompactionTransform::npos) &&
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

    // A row of the doubled matrix, kept as its two halves; `first` is null once
    // it is zero. A row of `left` is both of its own halves.
    struct DoubledRow {
        std::size_t pivot_col_idx;
        const Equation* first;
        const Equation* second;
    };
    std::vector<DoubledRow> doubled; // ascending in `pivot_col_idx`
    doubled.reserve(left.rows() + right.rows());
    const auto find = [&doubled](const std::size_t col_idx) {
        return std::lower_bound(doubled.begin(), doubled.end(), col_idx,
                                [](const DoubledRow& row, const std::size_t wanted) {
                                    return row.pivot_col_idx < wanted;
                                });
    };

    for (const Equation& row : left) {
        doubled.push_back({*row.leading_bit(), &row, &row});
    }

    // The halves of the rows `right` adds; reserved, so the pointers stay valid.
    std::vector<Equation> firsts;
    std::vector<Equation> seconds;
    firsts.reserve(right.rows());
    seconds.reserve(right.rows());
    for (const Equation& row : right) {
        Equation& first = firsts.emplace_back(row);
        Equation& second = seconds.emplace_back(Equation::with_num_bits(repr, cols));
        bool is_first_zero = false;
        while (true) {
            const std::optional<std::size_t> leading =
                is_first_zero ? second.leading_bit() : first.leading_bit();
            if (!leading.has_value()) {
                if (is_first_zero) {
                    break; // the row already belongs to the doubled span
                }
                is_first_zero = true;
                continue;
            }
            const std::size_t col_idx = is_first_zero ? cols + *leading : *leading;
            const auto found = find(col_idx);
            if (found == doubled.end() || found->pivot_col_idx != col_idx) {
                doubled.insert(found, {col_idx, is_first_zero ? nullptr : &first, &second});
                break;
            }
            if (!is_first_zero) {
                first.plus_assign(*found->first);
            }
            second.plus_assign(*found->second);
        }
    }

    std::vector<Equation> basis;
    for (auto row = find(cols); row != doubled.end(); ++row) {
        // Only a row `right` added has a zero first half, so its second is in `seconds`.
        basis.push_back(std::move(seconds[static_cast<std::size_t>(row->second - seconds.data())]));
    }
    return basis;
}

} // namespace xorcle
