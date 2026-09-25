#pragma once

#include <bit>
#include <cassert>
#include <cstddef>
#include <optional>
#include <span>

#include <gf2/gf2.h>

#include "algebra/equation_repr.h"
#include "util/index_set.h"

namespace xorcle {

/// @brief A bit-packed implementation of `GF2Vector` based on `gf2::BitVector`.
class DenseGF2Vector {
public:
    static constexpr EquationRepr repr = EquationRepr::dense;

    using BitVector = gf2::BitVector<gf2::u64>;
    using Word = BitVector::word_type;

    /// @brief Construct an all-zeros vector.
    explicit DenseGF2Vector(std::size_t num_bits) : bits_(num_bits) {}

    std::size_t num_bits() const noexcept { return bits_.size(); }

    /// @brief The words bit `i` is packed into, `i / 64` first; bits past
    /// `num_bits()` are zero.
    const Word* words() const noexcept { return bits_.store(); }

    std::size_t num_words() const noexcept { return bits_.words(); }

    /// @brief Resize to `num_bits` bits, zeroing any new ones.
    void resize(std::size_t num_bits) { bits_.resize(num_bits); }

    void set_zero() noexcept { gf2::set_all(bits_, false); }

    /// @brief Set the listed bits to 1 and the rest to 0.
    /// @pre Each bit must be below `num_bits()`.
    void assign_bits(std::span<const std::size_t> bits) {
        set_zero();
        for (const std::size_t bit : bits) {
            set_bit(bit);
        }
    }

    /// @pre `i < num_bits()`.
    bool get_bit(std::size_t i) const noexcept {
        assert(i < bits_.size() && "get_bit index is out of range");
        return bits_.get(i);
    }

    /// @pre `i < num_bits()`.
    void set_bit(std::size_t i) noexcept {
        assert(i < bits_.size() && "set_bit index is out of range");
        gf2::set(bits_, i, true);
    }

    /// @pre `i < num_bits()`.
    void reset_bit(std::size_t i) noexcept {
        assert(i < bits_.size() && "reset_bit index is out of range");
        gf2::set(bits_, i, false);
    }

    /// @brief Get index of the first set bit, or `std::nullopt` if none are set.
    /// Custom reimplementation of `gf2::first_set()` for efficiency.
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

    /// @brief Get index of the first set bit strictly after `i`, or `std::nullopt`.
    /// Custom reimplementation of `gf2::next_set()` for efficiency.
    /// @pre `i < num_bits()`.
    std::optional<std::size_t> next_bit(std::size_t i) const noexcept {
        assert(i < bits_.size() && "next_bit index is out of range");
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

    /// @brief Get index of the first set bit at or after `i` that `indices` also
    /// holds, or `std::nullopt`.
    /// @pre `indices.size() == num_bits()`.
    std::optional<std::size_t> first_bit_in(const IndexSet& indices,
                                            std::size_t i) const noexcept {
        assert(indices.size() == bits_.size() && "first_bit_in size mismatch");
        if (i >= bits_.size()) {
            return std::nullopt;
        }
        const Word* words = bits_.store();
        const std::size_t num_words = bits_.words();
        std::size_t w = i >> word_shift;
        Word word = words[w] & indices.word(w) & (~Word{0} << (i & word_mask));
        while (word == 0) {
            if (++w >= num_words) {
                return std::nullopt;
            }
            word = words[w] & indices.word(w);
        }
        return (w << word_shift) + static_cast<std::size_t>(std::countr_zero(word));
    }

    /// @brief Add another vector into this one over GF(2).
    /// Custom reimplementation of `gf2::operator^=()` for efficiency.
    /// @pre `other.num_bits() == num_bits()`.
    ///
    /// gf2's `operator^=` writes through `set_word`, which re-cleans the tail
    /// word on every call; a plain word loop does not.
    void plus_assign(const DenseGF2Vector& other) noexcept {
        assert(bits_.size() == other.bits_.size() && "bit count mismatch in plus_assign");
        Word* lhs = bits_.store();
        const Word* rhs = other.bits_.store();
        const std::size_t num_words = bits_.words();
        for (std::size_t i = 0; i < num_words; ++i) {
            lhs[i] ^= rhs[i];
        }
    }

    /// @brief Add `other` in and return the first set bit after `i`, in one pass
    /// over the words.
    /// @pre Neither vector has a set bit below `i`, `other.num_bits() ==
    /// num_bits()`, and `i < num_bits()`.
    std::optional<std::size_t> plus_assign_and_next_bit(const DenseGF2Vector& other,
                                                        std::size_t i) noexcept {
        assert(bits_.size() == other.bits_.size() && "bit count mismatch in plus_assign");
        assert(i < bits_.size() && "plus_assign_and_next_bit index is out of range");
        assert(no_set_bit_below(i) && other.no_set_bit_below(i) &&
               "plus_assign_and_next_bit starts below a set bit");
        Word* lhs = bits_.store();
        const Word* rhs = other.bits_.store();
        const std::size_t num_words = bits_.words();
        // Words below `i` are zero in both vectors, so the add can start there.
        const std::size_t first = i >> word_shift;
        const std::size_t next = i + 1;
        std::optional<std::size_t> found;
        for (std::size_t w = first; w < num_words; ++w) {
            const Word word = (lhs[w] ^= rhs[w]);
            if (found) {
                continue;
            }
            const Word above =
                (next >> word_shift) == w ? word & (~Word{0} << (next & word_mask)) : word;
            if (above != 0) {
                found = (w << word_shift) + static_cast<std::size_t>(std::countr_zero(above));
            }
        }
        return found;
    }

    /// @brief Overwrite from `other` without allocating.
    /// @pre `other.num_bits() == num_bits()`.
    void assign_from(const DenseGF2Vector& other) noexcept;

    bool equals(const DenseGF2Vector& other) const noexcept { return bits_ == other.bits_; }

    std::size_t hash() const noexcept;

    /// @brief Call `f(bit)` for each set bit, in ascending order.
    template <class F>
    void for_each_set_bit(F&& f) const {
        for (auto bit = leading_bit(); bit; bit = next_bit(*bit)) {
            f(*bit);
        }
    }

private:
#ifndef NDEBUG
    /// @brief Whether every bit below `i` is zero, for the fused add's precondition.
    bool no_set_bit_below(std::size_t i) const noexcept {
        const std::optional<std::size_t> first = leading_bit();
        return !first || *first >= i;
    }
#endif

    static constexpr std::size_t bits_per_word = sizeof(Word) * 8;

    /// @brief `log2(bits_per_word)`: shifts a bit index down to its word index.
    static constexpr std::size_t word_shift =
        static_cast<std::size_t>(std::countr_zero(bits_per_word));

    static constexpr std::size_t word_mask = bits_per_word - 1; ///< Bit offset within a word.

    BitVector bits_;
};

} // namespace xorcle
