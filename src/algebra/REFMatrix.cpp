#include "algebra/REFMatrix.h"

#include <cassert>

namespace algebra {
REFMatrix::REFMatrix(bit::matrix<> M) {
    this->M = M.to_echelon_form(&pivot_cols);
    col_to_row = std::vector<std::size_t>(M.cols());
    std::size_t row = 0;
    pivot_cols.if_set_call([&](std::size_t col) {
        col_to_row[col] = row;
        ++row;
    });
}

std::optional<std::size_t> REFMatrix::add_row_get_index(const bit::vector<>& v) {
    // TODO: Do we need this? May already be covered by remaining code
    if (M.rows() == 0) {
        pivot_cols.set(v.first_set());
        col_to_row[v.first_set()] = 0;
        M.push_row(v);
        return 0;
    }

    bit::vector<> reduced = v;

    // Reduce the vector up to the first irreducible column
    std::size_t row = 0;
    std::size_t col = 0;
    for (; col < pivot_cols.size(); ++col) {
        if (reduced.element(col)) {
            if (pivot_cols.element(col)) {
                reduced ^= M.row(row); // Equal to col_to_row[col]
            }
            else {
                break;
            }
        }
        row += pivot_cols.element(col);
    }

    // If the vector is now 0, don't do anything
    if (reduced.none()) {
        return std::nullopt;
    }

    // Insert the new row
    M.insert_row(row, reduced);
    pivot_cols.if_set_call([&](std::size_t i) {
        if (i > col) {
            ++col_to_row[i];
        }
    });
    pivot_cols.set(col);
    col_to_row[col] = row;
    return row;
}

void REFMatrix::add_row(const bit::vector<>& v) {
    add_row_get_index(v);
}

void REFMatrix::reduce(bit::vector<>& v, bit::vector<>* coeffs) const {
    if (coeffs) {
        *coeffs = bit::vector<>::zeros(M.rows());
        v.if_set_call([&](std::size_t i) { 
            if (pivot_cols.element(i)) {
                v ^= M.row(col_to_row[i]);
                coeffs->set(col_to_row[i]);
            }
        });
        return;
    } else {
        v.if_set_call([&](std::size_t i) {
            if (pivot_cols.element(i)) {
                v ^= M.row(col_to_row[i]);
            }
        });
        return;
    }
}

std::size_t REFMatrix::partial_reduce(bit::vector<>& v) const {
    while (true) {
        std::size_t f = v.first_set();
        if (f == bit::vector<>::npos) {
            return f;
        }
        if (!pivot_cols.element(f)) {
            return f;
        }
        v ^= M.row(col_to_row[f]);
    }
}

// Requires extra to be already partial reduced by the matrix (and may be 0 = 1).
std::size_t REFMatrix::partial_reduce_with_extra(bit::vector<>& v, const bit::vector<>& extra) const {
    std::size_t extra_col = extra.first_set();
    while (true) {
        std::size_t f = v.first_set();
        if (f == bit::vector<>::npos) {
            return f;
        }
        if (!pivot_cols.element(f)) {
            if (f == extra_col) {
                v ^= extra;
                continue;
            }
            return f;
        }
        assert(pivot_cols.element(f));
        v ^= M.row(col_to_row[f]);
    }
}

// bit::vector<> REFMatrix::reduce_copy(const bit::vector<>& v, bit::vector<>* coeffs) const {
//     bit::vector<> v_copy = v;
//     reduce(v_copy, coeffs);
//     return v_copy;
// }

void REFMatrix::reduce_with_extra(bit::vector<>& v, const bit::vector<>& extra) const {
    std::size_t extra_col = extra.first_set();
    v.if_set_call([&](std::size_t i) {
        if (i == extra_col) {
            v ^= extra;
        } else if (pivot_cols.element(i)) {
            v ^= M.row(col_to_row[i]);
        }
    });
    return;
}

// bit::vector<> REFMatrix::reduce_with_extra_copy(const bit::vector<>& v, const bit::vector<>& extra) const {
//     bit::vector<> v_copy = v;
//     reduce_with_extra(v_copy, extra);
//     return v_copy;
// }

bit::vector<> REFMatrix::pop_first_row() {
    bit::vector<> first_row = M.row(0);
    M.erase_row(0);
    pivot_cols.reset(pivot_cols.first_set());
    for (std::size_t i = 0; i < pivot_cols.size(); ++i) {
        if (pivot_cols.element(i)) {
            --col_to_row[i];
        }
    }
    return first_row;
}
} // namespace algebra
