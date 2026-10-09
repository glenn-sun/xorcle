#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "algebra/equation_repr.h"
#include "util/index_set.h"

namespace xorcle {

/// @brief A simple `GF2Vector` implementation as an ascending list of set bits.
class SparseGF2Vector {
public:
    static constexpr EquationRepr repr = EquationRepr::sparse;

    using Bit = std::uint32_t;

    /// @brief Zero: the bits are always the vector's own allocation.
    static constexpr std::size_t buffer_bytes(std::size_t) noexcept { return 0; }

    /// @brief Construct an all-zeros vector.
    explicit SparseGF2Vector(std::size_t num_bits) : num_bits_(num_bits) {}

    SparseGF2Vector(std::size_t num_bits, void*) : SparseGF2Vector(num_bits) {}

    SparseGF2Vector(const SparseGF2Vector& other, void*) : SparseGF2Vector(other) {}

    /// @brief Copies the bits, not the merge buffer.
    SparseGF2Vector(const SparseGF2Vector& other)
        : num_bits_(other.num_bits_), bits_(other.bits_) {}
    SparseGF2Vector& operator=(const SparseGF2Vector& other) {
        num_bits_ = other.num_bits_;
        bits_ = other.bits_;
        return *this;
    }
    SparseGF2Vector(SparseGF2Vector&&) noexcept = default;
    SparseGF2Vector& operator=(SparseGF2Vector&&) noexcept = default;

    std::size_t num_bits() const noexcept { return num_bits_; }

    /// @brief Drop any bit greater than or equal to `num_bits`.
    void resize(std::size_t num_bits);

    void set_zero() noexcept { bits_.clear(); }

    /// @brief Set the listed bits to 1 and the rest to 0.
    /// @pre Each bit must be below `num_bits()`.
    void assign_bits(std::span<const std::size_t> bits);

    /// @pre `i < num_bits()`.
    bool get_bit(std::size_t i) const noexcept {
        assert(i < num_bits_ && "get_bit index is out of range");
        return std::binary_search(bits_.begin(), bits_.end(), static_cast<Bit>(i));
    }

    /// @pre `i < num_bits()`.
    void set_bit(std::size_t i);

    /// @pre `i < num_bits()`.
    void reset_bit(std::size_t i);

    /// @brief Get index of the first set bit, or `std::nullopt` if none is set.
    std::optional<std::size_t> leading_bit() const noexcept {
        if (bits_.empty()) {
            return std::nullopt;
        }
        return bits_.front();
    }

    /// @brief Get index of the first set bit strictly after `i`, or `std::nullopt`.
    /// @pre `i < num_bits()`.
    std::optional<std::size_t> next_bit(std::size_t i) const noexcept {
        assert(i < num_bits_ && "next_bit index is out of range");
        const auto it = std::upper_bound(bits_.begin(), bits_.end(), static_cast<Bit>(i));
        if (it == bits_.end()) {
            return std::nullopt;
        }
        return *it;
    }

    /// @brief Get index of the first set bit at or after `i` that `indices` also
    /// holds, or `std::nullopt`.
    /// @pre `indices.size() == num_bits()`.
    std::optional<std::size_t> first_bit_in(const IndexSet& indices,
                                            std::size_t i) const noexcept {
        assert(indices.size() == num_bits_ && "first_bit_in size mismatch");
        for (auto it = std::lower_bound(bits_.begin(), bits_.end(), static_cast<Bit>(i));
             it != bits_.end(); ++it) {
            if (indices.test(*it)) {
                return *it;
            }
        }
        return std::nullopt;
    }

    /// @brief Add another vector into this one over GF(2).
    /// @pre `other.num_bits() == num_bits()`.
    void plus_assign(const SparseGF2Vector& other);

    /// @brief `plus_assign(other)`.
    /// @pre `other` has no set bit below `i`, and `other.num_bits() == num_bits()`.
    void plus_assign_from_bit(const SparseGF2Vector& other, std::size_t) { plus_assign(other); }

    /// @brief Add `other` in and return the first set bit after `i`.
    /// @pre Neither vector has a set bit below `i`, `other.num_bits() ==
    /// num_bits()`, and `i < num_bits()`.
    std::optional<std::size_t> plus_assign_and_next_bit(const SparseGF2Vector& other,
                                                        std::size_t i) {
        plus_assign(other);
        return next_bit(i);
    }

    /// @brief Overwrite from `other`, reusing this vector's buffer.
    /// @pre `other.num_bits() == num_bits()`.
    void assign_from(const SparseGF2Vector& other);

    bool equals(const SparseGF2Vector& other) const noexcept {
        return num_bits_ == other.num_bits_ && bits_ == other.bits_;
    }

    std::size_t hash() const noexcept;

    /// @brief Call `f(bit)` for each set bit, in ascending order.
    template <class F>
    void for_each_set_bit(F&& f) const {
        for (const Bit bit : bits_) {
            f(bit);
        }
    }

    std::size_t num_set_bits() const noexcept { return bits_.size(); }

    /// @brief Add each set bit to `indices`.
    /// @pre `indices.size() == num_bits()`.
    void add_set_bits_to(IndexSet& indices) const noexcept {
        assert(indices.size() == num_bits_ && "add_set_bits_to size mismatch");
        for (const Bit bit : bits_) {
            indices.set(bit);
        }
    }

private:
    std::size_t num_bits_;
    std::vector<Bit> bits_;
    std::vector<Bit> scratch_; ///< Merge buffer for `plus_assign`, reused across calls.
};

} // namespace xorcle
