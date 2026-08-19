#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "algebra/equation_repr.h"
#include "algebra/reduce_result.h"

namespace xorcle {

/// @brief The sorted-index `GF2Vector` implementation.
///
/// A vector is the ascending list of its set bits. This representation exists to
/// keep the algebra interface honest -- an interface designed against a single
/// implementation is reliably the wrong interface. It is expected to be slower
/// than the dense one, and runs the same unit tests.
class SparseGF2Vector {
public:
    static constexpr EquationRepr repr = EquationRepr::sparse;

    using Bit = std::uint32_t;

    /// @brief Construct the all-zero vector of `num_vars + 1` bits.
    explicit SparseGF2Vector(std::size_t num_bits) : num_bits_(num_bits) {}

    std::size_t num_bits() const noexcept { return num_bits_; }

    /// @brief Resize to `num_bits` bits, dropping any set bit at or above it.
    void resize(std::size_t num_bits);

    void zero() noexcept { bits_.clear(); }

    /// @brief Zero, then set each listed bit; each must be below `num_bits()`.
    void assign_bits(std::span<const std::size_t> bits);

    /// @pre `i < num_bits()`.
    bool test_bit(std::size_t i) const noexcept {
        assert(i < num_bits_);
        return std::binary_search(bits_.begin(), bits_.end(), static_cast<Bit>(i));
    }

    /// @pre `i < num_bits()`.
    void set_bit(std::size_t i);

    /// @brief First set bit, or `std::nullopt` if none is set.
    std::optional<std::size_t> leading_bit() const noexcept {
        if (bits_.empty()) {
            return std::nullopt;
        }
        return bits_.front();
    }

    /// @brief First set bit strictly after `i`, or `std::nullopt`.
    std::optional<std::size_t> next_bit(std::size_t i) const noexcept {
        const auto it = std::upper_bound(bits_.begin(), bits_.end(), static_cast<Bit>(i));
        if (it == bits_.end()) {
            return std::nullopt;
        }
        return *it;
    }

    bool is_zero() const noexcept { return bits_.empty(); }

    /// @brief Classify as @f$\mathbf{0}@f$, @f$\mathbf{1}@f$, or a leading bit,
    /// in one size-and-front check rather than separate emptiness and
    /// constant-bit queries. The constant bit is the highest, so a leading bit at
    /// `num_bits_ - 1` means it is the only set bit.
    ReduceResult classify() const noexcept {
        if (bits_.empty()) {
            return ReduceResult::eq_true();
        }
        const std::size_t leading = bits_.front();
        if (leading + 1 == num_bits_) {
            return ReduceResult::eq_false();
        }
        return ReduceResult::leading(leading);
    }

    /// @brief Add another vector of equal width into this one over GF(2).
    void plus_assign(const SparseGF2Vector& other);

    /// @brief Overwrite from `other`, of equal width, without allocating.
    void assign_from(const SparseGF2Vector& other);

    bool equals(const SparseGF2Vector& other) const noexcept {
        return num_bits_ == other.num_bits_ && bits_ == other.bits_;
    }

    std::size_t hash() const noexcept;

    /// @brief Call `f(bit)` for each set bit, in ascending order.
    template <class F>
    void for_each_bit(F&& f) const {
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
