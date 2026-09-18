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
        insert_row_idx += pivot_[leading_col_idx] ? 1 : 0;
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

} // namespace xorcle
