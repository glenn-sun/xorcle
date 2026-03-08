#include "algebra/REFMatrix.h"

#include <cassert>
#include <sstream>
#include <stdexcept>

namespace algebra {

#ifndef NDEBUG
void REFMatrix::debug_assert_valid_representation() const {
    assert(pivot_cols.size() == num_vars + 1);
    assert(col_to_row.size() == num_vars + 1);
    assert((rows() == 0 && M.cols() == 0) || M.cols() == cols());

    std::size_t expected_row_index = 0;
    std::size_t previous_pivot = bit::vector<>::npos;
    for (std::size_t row_index = 0; row_index < rows(); ++row_index) {
        const bit::vector<>& current_row = M.row(row_index);
        assert(current_row.size() == cols());

        const std::size_t leading_col = current_row.first_set();
        assert(leading_col != bit::vector<>::npos);
        assert(leading_col < cols());
        if (previous_pivot != bit::vector<>::npos) {
            assert(previous_pivot < leading_col);
        }
        assert(pivot_cols.element(leading_col));
        assert(col_to_row[leading_col] == row_index);

        previous_pivot = leading_col;
        ++expected_row_index;
    }

    pivot_cols.if_set_call([&](std::size_t pivot_col) {
        assert(expected_row_index > 0 || rows() == 0);
        assert(col_to_row[pivot_col] < rows());
        assert(M.row(col_to_row[pivot_col]).first_set() == pivot_col);
    });

    assert(expected_row_index == rows());
}
#endif

REFMatrix::REFMatrix(bit::matrix<> M) {
    if (M.cols() == 0) {
        throw std::invalid_argument("REFMatrix(bit::matrix): matrix must be non-empty");
    }
    num_vars = M.cols() - 1;
    this->M = M.to_echelon_form(&pivot_cols);
    col_to_row = std::vector<std::size_t>(cols(), 0);
    std::size_t row_index = 0;
    pivot_cols.if_set_call([&](std::size_t pivot_col) {
        col_to_row[pivot_col] = row_index;
        ++row_index;
    });

#ifndef NDEBUG
    debug_assert_valid_representation();
#endif
}

std::size_t REFMatrix::get_col_to_row(std::size_t i) const {
#ifndef NDEBUG
    assert(i < cols());
    assert(pivot_cols.element(i));
#endif
    return col_to_row[i];
}

std::optional<std::size_t> REFMatrix::add_row_get_index(const bit::vector<>& v) {
#ifndef NDEBUG
    assert(v.size() == cols());
#endif

    if (M.rows() == 0) {
        const std::size_t leading_col = v.first_set();
        if (leading_col == bit::vector<>::npos) {
            return std::nullopt;
        }
        pivot_cols.set(leading_col);
        col_to_row[leading_col] = 0;
        M.push_row(v);
#ifndef NDEBUG
        debug_assert_valid_representation();
#endif
        return 0;
    }

    bit::vector<> reduced_row = v;

    std::size_t insert_row_index = 0;
    std::size_t leading_col = 0;
    for (; leading_col <= num_vars; ++leading_col) {
        if (reduced_row.element(leading_col)) {
            if (pivot_cols.element(leading_col)) {
                reduced_row ^= M.row(insert_row_index);
            } else {
                break;
            }
        }
        insert_row_index += pivot_cols.element(leading_col);
    }

    if (reduced_row.none()) {
        return std::nullopt;
    }

    M.insert_row(insert_row_index, reduced_row);
    pivot_cols.if_set_call([&](std::size_t pivot_col) {
        if (pivot_col > leading_col) {
            ++col_to_row[pivot_col];
        }
    });
    pivot_cols.set(leading_col);
    col_to_row[leading_col] = insert_row_index;

#ifndef NDEBUG
    debug_assert_valid_representation();
#endif
    return insert_row_index;
}

void REFMatrix::add_row(const bit::vector<>& v) {
    add_row_get_index(v);
}

void REFMatrix::reduce(bit::vector<>& v, bit::vector<>* coeffs) const {
#ifndef NDEBUG
    assert(v.size() == cols());
#endif
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
#ifndef NDEBUG
    assert(v.size() == cols());
#endif
    std::size_t f = v.first_set();
    while (true) {
        if (f == bit::vector<>::npos) {
            return f;
        }
        if (!pivot_cols.element(f)) {
            return f;
        }
        v ^= M.row(col_to_row[f]);
        f = v.next_set(f);
    }
}

std::size_t REFMatrix::partial_reduce_with_extra(bit::vector<>& v, const bit::vector<>& extra) const {
#ifndef NDEBUG
    assert(v.size() == cols());
    assert(extra.size() == cols());
#endif
    const std::size_t extra_leading_col = extra.first_set();

#ifndef NDEBUG
    assert(extra_leading_col == bit::vector<>::npos || !pivot_cols.element(extra_leading_col));
#endif
    std::size_t f = v.first_set();
    while (true) {
        if (f == bit::vector<>::npos) {
            return f;
        }
        if (!pivot_cols.element(f)) {
            if (f == extra_leading_col) {
                v ^= extra;
                f = v.next_set(f);
                continue;
            }
            return f;
        }
        v ^= M.row(col_to_row[f]);
        f = v.next_set(f);
    }
}

void REFMatrix::reduce_with_extra(bit::vector<>& v, const bit::vector<>& extra) const {
#ifndef NDEBUG
    assert(v.size() == cols());
    assert(extra.size() == cols());
    bit::vector<> extra_reduced = extra;
    reduce(extra_reduced);
    assert(extra_reduced.none());
#endif

    const std::size_t extra_col = extra.first_set();
    v.if_set_call([&](std::size_t i) {
        if (i == extra_col) {
            v ^= extra;
        } else if (pivot_cols.element(i)) {
            v ^= M.row(col_to_row[i]);
        }
    });
    return;
}

bit::vector<> REFMatrix::pop_first_row() {
    if (rows() == 0) {
        throw std::runtime_error("REFMatrix::pop_first_row: matrix is empty");
    }

    bit::vector<> first_row = M.row(0);
    const std::size_t leading_col = first_row.first_set();
    if (leading_col == bit::vector<>::npos) {
        throw std::logic_error("REFMatrix::pop_first_row: stored row has no pivot");
    }

    M.erase_row(0);
    pivot_cols.reset(leading_col);
    pivot_cols.if_set_call([&](std::size_t pivot_col) { --col_to_row[pivot_col]; });

#ifndef NDEBUG
    debug_assert_valid_representation();
#endif
    return first_row;
}

void REFMatrix::pop_last_row() {
    if (rows() == 0) {
        throw std::runtime_error("REFMatrix::pop_last_row: matrix is empty");
    }

    const bit::vector<>& last_row = M.row(rows() - 1);
    const std::size_t leading_col = last_row.first_set();
    if (leading_col == bit::vector<>::npos) {
        throw std::logic_error("REFMatrix::pop_last_row: stored row has no pivot");
    }

    M.pop_row();
    pivot_cols.reset(leading_col);

#ifndef NDEBUG
    debug_assert_valid_representation();
#endif
}
} // namespace algebra
