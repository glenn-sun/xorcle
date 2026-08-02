#include "algebra/sparse/sparse_row.h"

namespace xorcle {

namespace {

/// @brief Fibonacci hashing multiplier, as used by the old tree's equation hash.
constexpr std::uint64_t hash_multiplier = 11400714819323198485ull;

} // namespace

void SparseRow::resize(std::size_t width) {
    if (width < width_) {
        const auto first_dropped =
            std::lower_bound(columns_.begin(), columns_.end(), static_cast<Column>(width));
        columns_.erase(first_dropped, columns_.end());
    }
    width_ = width;
}

void SparseRow::assign_columns(std::span<const std::size_t> columns) {
    columns_.clear();
    columns_.reserve(columns.size());
    for (const std::size_t column : columns) {
        assert(column < width_);
        columns_.push_back(static_cast<Column>(column));
    }
    std::sort(columns_.begin(), columns_.end());
    columns_.erase(std::unique(columns_.begin(), columns_.end()), columns_.end());
}

void SparseRow::set(std::size_t i) {
    assert(i < width_);
    const Column column = static_cast<Column>(i);
    const auto it = std::lower_bound(columns_.begin(), columns_.end(), column);
    if (it == columns_.end() || *it != column) {
        columns_.insert(it, column);
    }
}

void SparseRow::xor_assign(const SparseRow& other) {
    assert(width_ == other.width_ && "width mismatch in xor_assign");

    // Symmetric difference of two ascending index lists, merged into a
    // long-lived buffer so the common case does not allocate.
    scratch_.clear();
    std::size_t i = 0;
    std::size_t j = 0;
    while (i < columns_.size() && j < other.columns_.size()) {
        if (columns_[i] < other.columns_[j]) {
            scratch_.push_back(columns_[i++]);
        } else if (other.columns_[j] < columns_[i]) {
            scratch_.push_back(other.columns_[j++]);
        } else {
            ++i;
            ++j;
        }
    }
    while (i < columns_.size()) {
        scratch_.push_back(columns_[i++]);
    }
    while (j < other.columns_.size()) {
        scratch_.push_back(other.columns_[j++]);
    }
    columns_.swap(scratch_);
}

void SparseRow::assign_from(const SparseRow& other) {
    assert(width_ == other.width_ && "width mismatch in assign_from");
    // `assign` reuses the existing buffer whenever its capacity suffices, which
    // it always does once a scratch equation has been filled to its high-water
    // mark. Unlike the dense backend a sparse row's storage grows with its
    // weight, so freedom from allocation is a property of the reuse discipline
    // rather than of the width alone.
    columns_.assign(other.columns_.begin(), other.columns_.end());
}

std::size_t SparseRow::hash() const noexcept {
    std::uint64_t value = width_;
    value *= hash_multiplier;
    for (const Column column : columns_) {
        value ^= column;
        value *= hash_multiplier;
    }
    return static_cast<std::size_t>(value);
}

} // namespace xorcle
