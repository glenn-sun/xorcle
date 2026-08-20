#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "algebra/equation_repr.h"

namespace xorcle {

/// @brief A simple `GF2Vector` implementation as an ascending list of set bits.
class SparseGF2Vector {
public:
    static constexpr EquationRepr repr = EquationRepr::sparse;

    using Bit = std::uint32_t;

    /// @brief Construct an all-zeros vector.
    explicit SparseGF2Vector(std::size_t num_bits) : num_bits_(num_bits) {}

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

    /// @brief Add another vector into this one over GF(2).
    /// @pre `other.num_bits() == num_bits()`.
    void plus_assign(const SparseGF2Vector& other);

    /// @brief Overwrite from `other` without allocating.
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

private:
    std::size_t num_bits_;
    std::vector<Bit> bits_;
    std::vector<Bit> scratch_; ///< Merge buffer for `plus_assign`, kept to avoid allocating.
};

} // namespace xorcle
