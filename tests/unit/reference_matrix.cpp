#include "reference_matrix.h"

#include <span>

namespace xorcle {

std::optional<std::size_t> first_set(const Bits& bits) {
    for (std::size_t i = 0; i < bits.size(); ++i) {
        if (bits[i]) {
            return i;
        }
    }
    return std::nullopt;
}

std::optional<std::size_t> next_set(const Bits& bits, std::size_t after) {
    for (std::size_t i = after + 1; i < bits.size(); ++i) {
        if (bits[i]) {
            return i;
        }
    }
    return std::nullopt;
}

void xor_into(Bits& target, const Bits& source) {
    for (std::size_t i = 0; i < target.size(); ++i) {
        target[i] = target[i] != source[i];
    }
}

std::vector<std::size_t> set_columns(const Bits& bits) {
    std::vector<std::size_t> out;
    for (std::size_t i = 0; i < bits.size(); ++i) {
        if (bits[i]) {
            out.push_back(i);
        }
    }
    return out;
}

Equation to_equation(Algebra backend, const Bits& bits) {
    const std::vector<std::size_t> columns = set_columns(bits);
    return Equation::from_columns(backend, bits.size() - 1,
                                  std::span<const std::size_t>(columns));
}

Bits to_bits(const Equation& equation) {
    Bits out(equation.size(), false);
    for (const std::size_t column : columns_of(equation)) {
        out[column] = true;
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

bool none_set(const Bits& bits) { return !first_set(bits).has_value(); }

} // namespace

std::optional<std::size_t> ReferenceMatrix::add_row_get_index(const Bits& v) {
    if (rows_.empty()) {
        const std::optional<std::size_t> leading = first_set(v);
        if (!leading) {
            return std::nullopt;
        }
        pivot_[*leading] = true;
        column_to_row_[*leading] = 0;
        rows_.push_back(v);
        return 0;
    }

    Bits reduced = v;
    std::size_t insert_index = 0;
    std::size_t leading_column = 0;
    for (; leading_column <= num_vars_; ++leading_column) {
        if (reduced[leading_column]) {
            if (pivot_[leading_column]) {
                xor_into(reduced, rows_[insert_index]);
            } else {
                break;
            }
        }
        insert_index += pivot_[leading_column] ? 1 : 0;
    }

    if (none_set(reduced)) {
        return std::nullopt;
    }

    rows_.insert(rows_.begin() + static_cast<std::ptrdiff_t>(insert_index), reduced);
    for (std::size_t column = 0; column <= num_vars_; ++column) {
        if (pivot_[column] && column > leading_column) {
            ++column_to_row_[column];
        }
    }
    pivot_[leading_column] = true;
    column_to_row_[leading_column] = insert_index;
    return insert_index;
}

void ReferenceMatrix::reduce(Bits& v) const {
    for (auto column = first_set(v); column; column = next_set(v, *column)) {
        if (pivot_[*column]) {
            xor_into(v, rows_[column_to_row_[*column]]);
        }
    }
}

std::optional<std::size_t> ReferenceMatrix::partial_reduce(Bits& v) const {
    std::optional<std::size_t> column = first_set(v);
    while (true) {
        if (!column) {
            return std::nullopt;
        }
        if (!pivot_[*column]) {
            return column;
        }
        xor_into(v, rows_[column_to_row_[*column]]);
        column = next_set(v, *column);
    }
}

Bits ReferenceMatrix::pop_first_row() {
    const Bits first = rows_.front();
    const std::size_t leading = *first_set(first);
    rows_.erase(rows_.begin());
    pivot_[leading] = false;
    for (std::size_t column = 0; column <= num_vars_; ++column) {
        if (pivot_[column]) {
            --column_to_row_[column];
        }
    }
    return first;
}

void ReferenceMatrix::pop_last_row() {
    const std::size_t leading = *first_set(rows_.back());
    rows_.pop_back();
    pivot_[leading] = false;
}

} // namespace xorcle
