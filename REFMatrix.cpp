#include "REFMatrix.h"

REFMatrix::REFMatrix(bit::matrix<> M) {
    this->M = M.to_echelon_form(&pivot_cols);
    col_to_row = std::vector<std::size_t>(M.cols());
    std::size_t row = 0;
    pivot_cols.if_set_call([&](std::size_t col) {
        col_to_row[col] = row;
        ++row;
    });
}

void REFMatrix::add_row(const bit::vector<>& v, std::size_t* inserted_index) {
    if (M.rows() == 0) {
        pivot_cols = bit::vector<>::zeros(v.size());
        pivot_cols.set(v.first_set());
        col_to_row = std::vector<std::size_t>(v.size());
        col_to_row[v.first_set()] = 0;
        M.push_row(v);
        if (inserted_index) {
            *inserted_index = 0;
        }
        return;
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
        return;
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
    if (inserted_index) {
        *inserted_index = row;
    }

    return;
}

void REFMatrix::reduce(bit::vector<>& v, bit::vector<>* coeffs) const {
    if (coeffs) {
        *coeffs = bit::vector<>::zeros(M.rows());
        std::size_t row = 0;
        pivot_cols.if_set_call([&](std::size_t i) { 
            if (v.element(i)) {
                v ^= M.row(row);
                coeffs->set(row);
            }
            ++row;
        });
        return;
    } else {
        std::size_t row = 0;
        pivot_cols.if_set_call([&](std::size_t i) { 
            if (v.element(i)) {
                v ^= M.row(row);
            }
            ++row;
        });
        return;
    }
}

bit::vector<> REFMatrix::reduce_copy(const bit::vector<>& v, bit::vector<>* coeffs) const {
    bit::vector<> v_copy = v;
    reduce(v_copy, coeffs);
    return v_copy;
}

void REFMatrix::reduce_with_extra(bit::vector<>& v, const bit::vector<>& extra) const {
    std::size_t extra_col = extra.first_set();

    std::size_t row = 0;
    for (std::size_t i = 0; i < M.cols(); ++i) {
        if (i == extra_col && v.element(i)) {
            v ^= extra;
        } else if (pivot_cols.element(i)) {
            if (v.element(i)) {
                v ^= M.row(row);
            }
            ++row;
        }
    }
    return;
}

bit::vector<> REFMatrix::reduce_with_extra_copy(const bit::vector<>& v, const bit::vector<>& extra) const {
    bit::vector<> v_copy = v;
    reduce_with_extra(v_copy, extra);
    return v_copy;
}

bit::vector<> REFMatrix::pop_first_row() {
    bit::vector<> first_row = M.row(0);
    M.erase_row(0);
    pivot_cols.reset(pivot_cols.first_set());
    return first_row;
}