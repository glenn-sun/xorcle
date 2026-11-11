#include "algebra/sparse/sparse_gf2_vector.h"

namespace xorcle {

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
        assert(bit < num_bits_ && "assign_bits index is out of range");
        bits_.push_back(static_cast<Bit>(bit));
    }
    std::sort(bits_.begin(), bits_.end());
    bits_.erase(std::unique(bits_.begin(), bits_.end()), bits_.end());
}

void SparseGF2Vector::set_bit(std::size_t i) {
    assert(i < num_bits_ && "set_bit index is out of range");
    const Bit bit = static_cast<Bit>(i);
    const auto it = std::lower_bound(bits_.begin(), bits_.end(), bit);
    if (it == bits_.end() || *it != bit) {
        bits_.insert(it, bit);
    }
}

void SparseGF2Vector::reset_bit(std::size_t i) {
    assert(i < num_bits_ && "reset_bit index is out of range");
    const Bit bit = static_cast<Bit>(i);
    const auto it = std::lower_bound(bits_.begin(), bits_.end(), bit);
    if (it != bits_.end() && *it == bit) {
        bits_.erase(it);
    }
}

void SparseGF2Vector::flip_bit(std::size_t i) {
    assert(i < num_bits_ && "flip_bit index is out of range");
    const Bit bit = static_cast<Bit>(i);
    const auto it = std::lower_bound(bits_.begin(), bits_.end(), bit);
    if (it != bits_.end() && *it == bit) {
        bits_.erase(it);
    } else {
        bits_.insert(it, bit);
    }
}

void SparseGF2Vector::plus_assign(const SparseGF2Vector& other) {
    assert(num_bits_ == other.num_bits_ && "bit count mismatch in plus_assign");
    // Symmetric difference of two ascending bit lists
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
    assert(num_bits_ == other.num_bits_ && "bit count mismatch in assign_from");
    bits_.assign(other.bits_.begin(), other.bits_.end());
}

namespace {

/// @brief Fibonacci hash of `bits`, seeded with the bit count.
std::uint64_t hash_bits(const std::size_t num_bits,
                        const std::span<const SparseGF2Vector::Bit> bits) noexcept {
    std::uint64_t value = num_bits;
    value *= 11400714819323198485ull;
    for (const SparseGF2Vector::Bit bit : bits) {
        value ^= bit;
        value *= 11400714819323198485ull;
    }
    return value;
}

} // namespace

std::span<const SparseGF2Vector::Bit> SparseGF2Vector::bits_except_last_bit() const noexcept {
    const bool last_is_set = !bits_.empty() && bits_.back() + 1 == num_bits_;
    return std::span<const Bit>(bits_.data(), bits_.size() - (last_is_set ? 1 : 0));
}

bool SparseGF2Vector::equals_except_last_bit(const SparseGF2Vector& other) const noexcept {
    assert(num_bits_ != 0 && num_bits_ == other.num_bits_ &&
           "bit count mismatch in equals_except_last_bit");
    return std::ranges::equal(bits_except_last_bit(), other.bits_except_last_bit());
}

std::size_t SparseGF2Vector::hash() const noexcept {
    return static_cast<std::size_t>(hash_bits(num_bits_, bits_));
}

std::size_t SparseGF2Vector::hash_except_last_bit() const noexcept {
    assert(num_bits_ != 0 && "hash_except_last_bit on a vector of no bits");
    return static_cast<std::size_t>(hash_bits(num_bits_, bits_except_last_bit()));
}

} // namespace xorcle
