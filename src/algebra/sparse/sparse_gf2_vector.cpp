#include "algebra/sparse/sparse_gf2_vector.h"

namespace xorcle {

namespace {

/// @brief Fibonacci hashing multiplier.
constexpr std::uint64_t hash_multiplier = 11400714819323198485ull;

} // namespace

void SparseGF2Vector::resize(std::size_t num_bits) {
    if (num_bits < num_bits_) {
        const auto first_dropped =
            std::lower_bound(bits_.begin(), bits_.end(), static_cast<Bit>(num_bits));
        bits_.erase(first_dropped, bits_.end());
    }
    num_bits_ = num_bits;
}

void SparseGF2Vector::assign_bits(std::span<const std::size_t> bits) {
    bits_.clear();
    bits_.reserve(bits.size());
    for (const std::size_t bit : bits) {
        assert(bit < num_bits_);
        bits_.push_back(static_cast<Bit>(bit));
    }
    std::sort(bits_.begin(), bits_.end());
    bits_.erase(std::unique(bits_.begin(), bits_.end()), bits_.end());
}

void SparseGF2Vector::set_bit(std::size_t i) {
    assert(i < num_bits_);
    const Bit bit = static_cast<Bit>(i);
    const auto it = std::lower_bound(bits_.begin(), bits_.end(), bit);
    if (it == bits_.end() || *it != bit) {
        bits_.insert(it, bit);
    }
}

void SparseGF2Vector::plus_assign(const SparseGF2Vector& other) {
    assert(num_bits_ == other.num_bits_ && "width mismatch in plus_assign");

    // Symmetric difference of two ascending index lists, merged into a
    // long-lived buffer so the common case does not allocate.
    scratch_.clear();
    std::size_t i = 0;
    std::size_t j = 0;
    while (i < bits_.size() && j < other.bits_.size()) {
        if (bits_[i] < other.bits_[j]) {
            scratch_.push_back(bits_[i++]);
        } else if (other.bits_[j] < bits_[i]) {
            scratch_.push_back(other.bits_[j++]);
        } else {
            ++i;
            ++j;
        }
    }
    while (i < bits_.size()) {
        scratch_.push_back(bits_[i++]);
    }
    while (j < other.bits_.size()) {
        scratch_.push_back(other.bits_[j++]);
    }
    bits_.swap(scratch_);
}

void SparseGF2Vector::assign_from(const SparseGF2Vector& other) {
    assert(num_bits_ == other.num_bits_ && "width mismatch in assign_from");
    // `assign` reuses the existing buffer whenever its capacity suffices, which
    // it always does once a scratch equation has been filled to its high-water
    // mark. A sparse vector's storage grows with its weight, not its width, so
    // freedom from allocation here is a property of the reuse discipline.
    bits_.assign(other.bits_.begin(), other.bits_.end());
}

std::size_t SparseGF2Vector::hash() const noexcept {
    std::uint64_t value = num_bits_;
    value *= hash_multiplier;
    for (const Bit bit : bits_) {
        value ^= bit;
        value *= hash_multiplier;
    }
    return static_cast<std::size_t>(value);
}

} // namespace xorcle
