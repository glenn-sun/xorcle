#pragma once

#include <bit>
#include <cassert>
#include <cstddef>
#include <optional>
#include <span>

#include <gf2/gf2.h>

#include "algebra/equation_repr.h"
#include "algebra/reduce_result.h"

namespace xorcle {

/// @brief The bit-packed `GF2Vector` implementation, on `gf2::BitVector`.
///
/// This header and `dense_gf2_vector.cpp` are the only files permitted to
/// include gf2. The word type is
/// spelled explicitly because `gf2::BitVector<>` defaults to `std::size_t`,
/// which here is `unsigned long` while `gf2::u64` is `unsigned long long`; gf2's
/// binary operators require identical word types, so mixing the two silently
/// fails to compile or compares unequal.
class DenseGF2Vector {
public:
    static constexpr EquationRepr repr = EquationRepr::dense;

    using Storage = gf2::BitVector<gf2::u64>;
    using Word = Storage::word_type;

    /// @brief Construct the all-zero vector of `num_vars + 1` bits.
    explicit DenseGF2Vector(std::size_t num_bits) : bits_(num_bits) {}

    std::size_t num_bits() const noexcept { return bits_.size(); }

    /// @brief Resize to `num_bits` bits, zeroing any new ones.
    void resize(std::size_t num_bits) { bits_.resize(num_bits); }

    void zero() noexcept { gf2::set_all(bits_, false); }

    /// @brief Zero, then set each listed bit; each must be below `num_bits()`.
    void assign_bits(std::span<const std::size_t> bits) {
        zero();
        for (const std::size_t bit : bits) {
            set_bit(bit);
        }
    }

    /// @pre `i < num_bits()`.
    bool test_bit(std::size_t i) const noexcept { return bits_.get(i); }

    /// @pre `i < num_bits()`.
    void set_bit(std::size_t i) noexcept { gf2::set(bits_, i, true); }

    /// @brief First set bit, or `std::nullopt` if none is set. A word scan over
    /// `store()`, for the reason given on `plus_assign`.
    std::optional<std::size_t> leading_bit() const noexcept {
        const Word* words = bits_.store();
        const std::size_t num_words = bits_.words();
        for (std::size_t w = 0; w < num_words; ++w) {
            if (words[w] != 0) {
                return (w << word_shift) + static_cast<std::size_t>(std::countr_zero(words[w]));
            }
        }
        return std::nullopt;
    }

    /// @brief First set bit strictly after `i`, or `std::nullopt`.
    ///
    /// A word scan over `store()` rather than gf2's `next_set`. The two agree
    /// exactly, but gf2 masks the starting word inside its scan loop, via a
    /// `reset_bits` call guarded by an `i == word_index` test, and re-checks the
    /// word index against `words()` having already checked the bit index against
    /// `size()`. Written out, the mask is one branch-free shift and the loop
    /// carries no per-word conditional -- and this runs once per row operation
    /// in the reduction loop, where that is a measurable share of the iteration.
    /// @pre `i < num_bits()`.
    std::optional<std::size_t> next_bit(std::size_t i) const noexcept {
        const std::size_t next = i + 1;
        if (next >= bits_.size()) {
            return std::nullopt;
        }
        const Word* words = bits_.store();
        const std::size_t num_words = bits_.words();
        std::size_t w = next >> word_shift;
        Word word = words[w] & (~Word{0} << (next & word_mask));
        while (word == 0) {
            if (++w >= num_words) {
                return std::nullopt;
            }
            word = words[w];
        }
        return (w << word_shift) + static_cast<std::size_t>(std::countr_zero(word));
    }

    bool is_zero() const noexcept { return bits_.none(); }

    /// @brief Classify as @f$\mathbf{0}@f$, @f$\mathbf{1}@f$, or a leading bit,
    /// in one word scan rather than separate emptiness and constant-bit queries:
    /// a leading bit at `num_bits() - 1` can only mean the constant bit alone is
    /// set.
    ReduceResult classify() const noexcept {
        const std::optional<std::size_t> leading = leading_bit();
        if (!leading) {
            return ReduceResult::eq_true();
        }
        if (*leading + 1 == bits_.size()) {
            return ReduceResult::eq_false();
        }
        return ReduceResult::leading(*leading);
    }

    /// @brief Add another vector into this one over GF(2).
    ///
    /// A word loop over `store()` rather than `bits_ ^= other.bits_`. gf2's
    /// `operator^=` writes each word through `set_word`, which re-`clean()`s the
    /// final word on every call -- a read-modify-write plus a `size() % 64`
    /// branch, taken essentially always for an @f$n+1@f$-bit equation. Adding
    /// two vectors that already satisfy gf2's zero-unused-bits invariant can
    /// never violate it, so that work is pure overhead, and it is paid once per
    /// row operation in the reduction loop, the solver's hottest inner loop,
    /// where it measured at roughly 25% of total runtime. `store()` and
    /// `words()` are public parts of gf2's `BitStore` concept, so this stays
    /// within gf2's interface and inside `algebra/dense/`.
    /// @pre `other.num_bits() == num_bits()`.
    void plus_assign(const DenseGF2Vector& other) noexcept {
        assert(bits_.size() == other.bits_.size());
        Word* lhs = bits_.store();
        const Word* rhs = other.bits_.store();
        const std::size_t num_words = bits_.words();
        for (std::size_t i = 0; i < num_words; ++i) {
            lhs[i] ^= rhs[i];
        }
    }

    /// @brief Overwrite from `other`, of equal width, without allocating.
    void assign_from(const DenseGF2Vector& other) noexcept;

    bool equals(const DenseGF2Vector& other) const noexcept { return bits_ == other.bits_; }

    std::size_t hash() const noexcept;

    /// @brief Call `f(bit)` for each set bit, in ascending order.
    template <class F>
    void for_each_bit(F&& f) const {
        for (auto bit = leading_bit(); bit; bit = next_bit(*bit)) {
            f(*bit);
        }
    }

private:
    static constexpr std::size_t bits_per_word = sizeof(Word) * 8;

    /// @brief `log2(bits_per_word)`: shifts a bit index down to its word index.
    static constexpr std::size_t word_shift =
        static_cast<std::size_t>(std::countr_zero(bits_per_word));

    static constexpr std::size_t word_mask = bits_per_word - 1; ///< Bit offset within a word.

    Storage bits_;
};

} // namespace xorcle
