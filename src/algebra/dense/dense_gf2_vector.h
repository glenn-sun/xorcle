#pragma once

#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>


#include "algebra/equation_repr.h"
#include "util/index_set.h"

namespace xorcle {

/// @brief A bit-packed implementation of `GF2Vector`.
///
/// The words are its own allocation, or a buffer its holder allocates beside it.
class DenseGF2Vector {
public:
    static constexpr EquationRepr repr = EquationRepr::dense;

    using Word = std::uint64_t;

    static constexpr std::size_t bits_per_word = sizeof(Word) * 8;

    /// @brief `log2(bits_per_word)`: shifts a bit index down to its word index.
    static constexpr std::size_t word_shift =
        static_cast<std::size_t>(std::countr_zero(bits_per_word));

    static constexpr std::size_t word_mask = bits_per_word - 1; ///< Bit offset within a word.

    /// @brief The size of the buffer that holds the words of `num_bits` bits.
    static constexpr std::size_t buffer_bytes(std::size_t num_bits) noexcept {
        return word_count(num_bits) * sizeof(Word);
    }

    /// @brief Construct an all-zeros vector.
    explicit DenseGF2Vector(std::size_t num_bits)
        : num_bits_(num_bits),
          capacity_(word_count(num_bits)),
          words_(new Word[capacity_]()),
          owns_words_(true) {}

    /// @brief Construct an all-zeros vector whose words are kept in `buffer`.
    /// @pre `buffer` has `buffer_bytes(num_bits)` bytes aligned for `Word`, and
    /// outlives the vector.
    DenseGF2Vector(std::size_t num_bits, void* buffer) noexcept
        : num_bits_(num_bits),
          capacity_(word_count(num_bits)),
          words_(static_cast<Word*>(buffer)),
          owns_words_(false) {
        std::memset(words_, 0, capacity_ * sizeof(Word));
    }

    DenseGF2Vector(const DenseGF2Vector& other)
        : num_bits_(other.num_bits_),
          capacity_(word_count(other.num_bits_)),
          words_(new Word[capacity_]),
          owns_words_(true) {
        std::memcpy(words_, other.words_, capacity_ * sizeof(Word));
    }

    /// @brief Construct a copy of `other` whose words are kept in `buffer`.
    /// @pre `buffer` has `buffer_bytes(other.num_bits())` bytes aligned for `Word`,
    /// and outlives the vector.
    DenseGF2Vector(const DenseGF2Vector& other, void* buffer) noexcept
        : num_bits_(other.num_bits_),
          capacity_(word_count(other.num_bits_)),
          words_(static_cast<Word*>(buffer)),
          owns_words_(false) {
        std::memcpy(words_, other.words_, capacity_ * sizeof(Word));
    }

    DenseGF2Vector& operator=(const DenseGF2Vector& other) {
        if (this != &other) {
            resize(other.num_bits_);
            std::memcpy(words_, other.words_, num_words() * sizeof(Word));
        }
        return *this;
    }

    ~DenseGF2Vector() {
        if (owns_words_) {
            delete[] words_;
        }
    }

    std::size_t num_bits() const noexcept { return num_bits_; }

    /// @brief The words bit `i` is packed into, `i / 64` first; bits past
    /// `num_bits()` are zero.
    const Word* words() const noexcept { return words_; }

    std::size_t num_words() const noexcept { return word_count(num_bits_); }

    /// @brief Resize to `num_bits` bits, zeroing any new ones.
    void resize(std::size_t num_bits);

    void set_zero() noexcept { std::memset(words_, 0, num_words() * sizeof(Word)); }

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
        assert(i < num_bits_ && "get_bit index is out of range");
        return ((words_[i >> word_shift] >> (i & word_mask)) & 1U) != 0;
    }

    /// @pre `i < num_bits()`.
    void set_bit(std::size_t i) noexcept {
        assert(i < num_bits_ && "set_bit index is out of range");
        words_[i >> word_shift] |= Word{1} << (i & word_mask);
    }

    /// @pre `i < num_bits()`.
    void reset_bit(std::size_t i) noexcept {
        assert(i < num_bits_ && "reset_bit index is out of range");
        words_[i >> word_shift] &= ~(Word{1} << (i & word_mask));
    }

    /// @brief Get index of the first set bit, or `std::nullopt` if none are set.
    std::optional<std::size_t> leading_bit() const noexcept {
        const Word* words = words_;
        const std::size_t num_words = word_count(num_bits_);
        for (std::size_t w = 0; w < num_words; ++w) {
            if (words[w] != 0) {
                return (w << word_shift) + static_cast<std::size_t>(std::countr_zero(words[w]));
            }
        }
        return std::nullopt;
    }

    /// @brief Get index of the first set bit strictly after `i`, or `std::nullopt`.
    /// @pre `i < num_bits()`.
    std::optional<std::size_t> next_bit(std::size_t i) const noexcept {
        assert(i < num_bits_ && "next_bit index is out of range");
        const std::size_t next = i + 1;
        if (next >= num_bits_) {
            return std::nullopt;
        }
        const Word* words = words_;
        const std::size_t num_words = word_count(num_bits_);
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
        assert(indices.size() == num_bits_ && "first_bit_in size mismatch");
        if (i >= num_bits_) {
            return std::nullopt;
        }
        const Word* words = words_;
        const std::size_t num_words = word_count(num_bits_);
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
    /// @pre `other.num_bits() == num_bits()`.
    void plus_assign(const DenseGF2Vector& other) noexcept {
        assert(num_bits_ == other.num_bits_ && "bit count mismatch in plus_assign");
        Word* lhs = words_;
        const Word* rhs = other.words_;
        const std::size_t num_words = word_count(num_bits_);
        for (std::size_t i = 0; i < num_words; ++i) {
            lhs[i] ^= rhs[i];
        }
    }

    /// @brief Add `other` in, reading only its words from bit `i` on.
    /// @pre `other` has no set bit below `i`, `other.num_bits() == num_bits()`, and
    /// `i < num_bits()`.
    void plus_assign_from_bit(const DenseGF2Vector& other, std::size_t i) noexcept {
        assert(num_bits_ == other.num_bits_ && "bit count mismatch in plus_assign_from_bit");
        assert(i < num_bits_ && "plus_assign_from_bit index is out of range");
        assert(other.no_set_bit_below(i) && "plus_assign_from_bit starts above a set bit");
        Word* lhs = words_;
        const Word* rhs = other.words_;
        const std::size_t num_words = word_count(num_bits_);
        for (std::size_t w = i >> word_shift; w < num_words; ++w) {
            lhs[w] ^= rhs[w];
        }
    }

    /// @brief Add `other` in and return the first set bit after `i`, in one pass
    /// over the words.
    /// @pre Neither vector has a set bit below `i`, `other.num_bits() ==
    /// num_bits()`, and `i < num_bits()`.
    std::optional<std::size_t> plus_assign_and_next_bit(const DenseGF2Vector& other,
                                                        std::size_t i) noexcept {
        assert(num_bits_ == other.num_bits_ && "bit count mismatch in plus_assign");
        assert(i < num_bits_ && "plus_assign_and_next_bit index is out of range");
        assert(no_set_bit_below(i) && other.no_set_bit_below(i) &&
               "plus_assign_and_next_bit starts below a set bit");
        Word* lhs = words_;
        const Word* rhs = other.words_;
        const std::size_t num_words = word_count(num_bits_);
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
    void assign_from(const DenseGF2Vector& other) noexcept {
        assert(num_bits_ == other.num_bits_ && "bit count mismatch in assign_from");
        const std::size_t num_words = word_count(num_bits_);
        // Below 8 words a plain word loop beats `std::memcpy`.
        if (num_words < 8) {
            Word* destination = words_;
            const Word* source = other.words_;
            for (std::size_t i = 0; i < num_words; ++i) {
                destination[i] = source[i];
            }
        } else {
            std::memcpy(words_, other.words_, num_words * sizeof(Word));
        }
    }

    bool equals(const DenseGF2Vector& other) const noexcept {
        return num_bits_ == other.num_bits_ &&
               std::memcmp(words_, other.words_, num_words() * sizeof(Word)) == 0;
    }

    std::size_t hash() const noexcept;

    /// @brief Call `f(bit)` for each set bit, in ascending order.
    /// @pre `f` does not change this vector.
    template <class F>
    void for_each_set_bit(F&& f) const {
        const Word* words = words_;
        const std::size_t num_words = word_count(num_bits_);
        for (std::size_t w = 0; w < num_words; ++w) {
            for (Word word = words[w]; word != 0; word &= word - 1) {
                f((w << word_shift) + static_cast<std::size_t>(std::countr_zero(word)));
            }
        }
    }

    std::size_t num_set_bits() const noexcept {
        const Word* words = words_;
        const std::size_t num_words = word_count(num_bits_);
        std::size_t count = 0;
        for (std::size_t w = 0; w < num_words; ++w) {
            count += static_cast<std::size_t>(std::popcount(words[w]));
        }
        return count;
    }

    /// @brief Add each set bit to `indices`.
    /// @pre `indices.size() == num_bits()`.
    void add_set_bits_to(IndexSet& indices) const noexcept {
        assert(indices.size() == num_bits_ && "add_set_bits_to size mismatch");
        indices.or_with_words({words_, num_words()});
    }

private:
#ifndef NDEBUG
    /// @brief Whether every bit below `i` is zero, for the fused add's precondition.
    bool no_set_bit_below(std::size_t i) const noexcept {
        const std::optional<std::size_t> first = leading_bit();
        return !first || *first >= i;
    }
#endif

    static constexpr std::size_t word_count(std::size_t num_bits) noexcept {
        return (num_bits + word_mask) >> word_shift;
    }

    std::size_t num_bits_;
    std::size_t capacity_; ///< In words.
    Word* words_;
    bool owns_words_; ///< False while the words are in a buffer the holder allocated.
};

} // namespace xorcle
