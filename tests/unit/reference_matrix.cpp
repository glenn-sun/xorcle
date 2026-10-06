#include "reference_matrix.h"

#include <cassert>
#include <span>

namespace xorcle {

std::vector<std::size_t> set_bits(const Bits& bits) {
    std::vector<std::size_t> out;
    for (std::size_t i = 0; i < bits.size(); ++i) {
        if (bits[i]) {
            out.push_back(i);
        }
    }
    return out;
}

Equation to_equation(EquationRepr repr, const Bits& bits) {
    const std::vector<std::size_t> indices = set_bits(bits);
    return Equation::from_bits(repr, bits.size() - 1,
                               std::span<const std::size_t>(indices));
}

Bits to_bits(const Equation& equation) {
    Bits out(equation.num_bits(), false);
    for (const std::size_t bit : bits_of(equation)) {
        out[bit] = true;
    }
    return out;
}

Bits random_bits(TestRandom& random, std::size_t num_vars, std::size_t density) {
    Bits out(num_vars + 1, false);
    for (std::size_t i = 0; i <= num_vars; ++i) {
        out[i] = random.chance(density);
    }
    return out;
}

ReduceResult expected_result(const std::optional<std::size_t>& leading, std::size_t num_vars) {
    if (!leading) {
        return ReduceResult::eq_true();
    }
    return *leading == num_vars ? ReduceResult::eq_false() : ReduceResult::leading(*leading);
}

namespace {

std::optional<std::size_t> first_set(const Bits& bits) {
    for (std::size_t i = 0; i < bits.size(); ++i) {
        if (bits[i]) {
            return i;
        }
    }
    return std::nullopt;
}

/// @brief First set bit of `bits` strictly after `after`, or `std::nullopt`.
std::optional<std::size_t> next_set(const Bits& bits, std::size_t after) {
    for (std::size_t i = after + 1; i < bits.size(); ++i) {
        if (bits[i]) {
            return i;
        }
    }
    return std::nullopt;
}

void plus_into(Bits& target, const Bits& source) {
    for (std::size_t i = 0; i < target.size(); ++i) {
        target[i] = target[i] != source[i];
    }
}

bool none_set(const Bits& bits) { return !first_set(bits).has_value(); }

} // namespace

std::optional<std::size_t> ReferenceMatrix::add_row_get_row_idx(const Bits& v) {
    if (rows_.empty()) {
        const std::optional<std::size_t> leading = first_set(v);
        if (!leading) {
            return std::nullopt;
        }
        pivot_[*leading] = true;
        col_idx_to_row_idx_[*leading] = 0;
        rows_.push_back(v);
        return 0;
    }

    Bits reduced = v;
    std::size_t insert_row_idx = 0;
    std::size_t leading_col_idx = 0;
    for (; leading_col_idx <= num_vars_; ++leading_col_idx) {
        if (reduced[leading_col_idx]) {
            if (pivot_[leading_col_idx]) {
                plus_into(reduced, rows_[insert_row_idx]);
            } else {
                break;
            }
        }
        insert_row_idx += pivot_[leading_col_idx] ? 1U : 0U;
    }

    if (none_set(reduced)) {
        return std::nullopt;
    }

    rows_.insert(rows_.begin() + static_cast<std::ptrdiff_t>(insert_row_idx), reduced);
    for (std::size_t col_idx = 0; col_idx <= num_vars_; ++col_idx) {
        if (pivot_[col_idx] && col_idx > leading_col_idx) {
            ++col_idx_to_row_idx_[col_idx];
        }
    }
    pivot_[leading_col_idx] = true;
    col_idx_to_row_idx_[leading_col_idx] = insert_row_idx;
    return insert_row_idx;
}

void ReferenceMatrix::reduce(Bits& v) const {
    for (auto col_idx = first_set(v); col_idx; col_idx = next_set(v, *col_idx)) {
        if (pivot_[*col_idx]) {
            plus_into(v, rows_[col_idx_to_row_idx_[*col_idx]]);
        }
    }
}

std::optional<std::size_t> ReferenceMatrix::partial_reduce(Bits& v) const {
    std::optional<std::size_t> col_idx = first_set(v);
    while (true) {
        if (!col_idx) {
            return std::nullopt;
        }
        if (!pivot_[*col_idx]) {
            return col_idx;
        }
        plus_into(v, rows_[col_idx_to_row_idx_[*col_idx]]);
        col_idx = next_set(v, *col_idx);
    }
}

Bits ReferenceMatrix::pop_first_row() {
    assert(rows() > 0 && "pop_first_row on an empty matrix");
    const Bits first = rows_.front();
    const std::size_t leading = *first_set(first);
    rows_.erase(rows_.begin());
    pivot_[leading] = false;
    for (std::size_t col_idx = 0; col_idx <= num_vars_; ++col_idx) {
        if (pivot_[col_idx]) {
            --col_idx_to_row_idx_[col_idx];
        }
    }
    return first;
}

void ReferenceMatrix::pop_last_row() {
    assert(rows() > 0 && "pop_last_row on an empty matrix");
    const std::size_t leading = *first_set(rows_.back());
    rows_.pop_back();
    pivot_[leading] = false;
}

Bits ReferenceMatrix::pop_row(std::size_t row_idx) {
    assert(row_idx < rows() && "pop_row index is out of range");
    Bits popped = rows_[row_idx];
    rows_.erase(rows_.begin() + static_cast<std::ptrdiff_t>(row_idx));
    index_pivots();
    return popped;
}

void ReferenceMatrix::add_to_row(std::size_t row_idx, const Bits& v) {
    assert(row_idx < rows() && "add_to_row index is out of range");
    assert(first_set(v) > first_set(rows_[row_idx]) && "the addend must lead after the row");
    plus_into(rows_[row_idx], v);
}

void ReferenceMatrix::reduce_rows() {
    // From the last row up, so each row added in is already reduced.
    for (std::size_t pivot_row_idx = rows_.size(); pivot_row_idx-- > 0;) {
        const std::size_t col_idx = *first_set(rows_[pivot_row_idx]);
        for (std::size_t row_idx = 0; row_idx < rows_.size(); ++row_idx) {
            if (row_idx != pivot_row_idx && rows_[row_idx][col_idx]) {
                plus_into(rows_[row_idx], rows_[pivot_row_idx]);
            }
        }
    }
}

std::vector<std::size_t> ReferenceMatrix::carried_col_idxs() const {
    std::vector<std::size_t> carried;
    for (std::size_t col_idx = 0; col_idx <= num_vars_; ++col_idx) {
        for (const Bits& row : rows_) {
            if (row[col_idx]) {
                carried.push_back(col_idx);
                break;
            }
        }
    }
    return carried;
}

void ReferenceMatrix::index_pivots() {
    pivot_.assign(num_vars_ + 1, false);
    for (std::size_t row_idx = 0; row_idx < rows_.size(); ++row_idx) {
        const std::size_t col_idx = *first_set(rows_[row_idx]);
        pivot_[col_idx] = true;
        col_idx_to_row_idx_[col_idx] = row_idx;
    }
}

} // namespace xorcle
