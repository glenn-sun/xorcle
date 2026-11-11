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

    /// @brief The words a vector of @p num_bits bits stores.
    static constexpr std::size_t word_count(std::size_t num_bits) noexcept {
        return (num_bits + word_mask) >> word_shift;
    }

    /// @brief The size of the buffer that holds the words of `num_bits` bits.
    static constexpr std::size_t buffer_bytes(std::size_t num_bits) noexcept {
        return word_count(num_bits) * sizeof(Word);
    }

    /// @brief The first set bit of @p num_words words, or `std::nullopt` if none is set.
    static std::optional<std::size_t> leading_bit(const Word* words,
                                                  std::size_t num_words) noexcept {
        for (std::size_t w = 0; w < num_words; ++w) {
            if (words[w] != 0) {
                return (w << word_shift) + static_cast<std::size_t>(std::countr_zero(words[w]));
            }
        }
        return std::nullopt;
    }

    /// @brief Copy @p num_words words.
    /// @pre `num_words >= 1`.
    static void copy_words(Word* to, const Word* from, std::size_t num_words) noexcept {
        assert(num_words != 0 && "copy_words of no words");
        // Up to 8 words, two overlapping fixed-size `std::memcpy`s.
        if (num_words <= 2) {
            to[0] = from[0];
            to[num_words - 1] = from[num_words - 1];
        } else if (num_words <= 4) {
            std::memcpy(to, from, 2 * sizeof(Word));
            std::memcpy(to + num_words - 2, from + num_words - 2, 2 * sizeof(Word));
        } else if (num_words <= 8) {
            std::memcpy(to, from, 4 * sizeof(Word));
            std::memcpy(to + num_words - 4, from + num_words - 4, 4 * sizeof(Word));
        } else {
            std::memcpy(to, from, num_words * sizeof(Word));
        }
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

    /// @brief Take `other`'s words, leaving it of no bits.
    /// @pre `other`'s words are its own allocation, not a holder's buffer.
    DenseGF2Vector(DenseGF2Vector&& other) noexcept
        : num_bits_(other.num_bits_),
          capacity_(other.capacity_),
          words_(other.words_),
          owns_words_(true) {
        assert(other.owns_words_ && "a vector is moved out of its holder's buffer");
        other.release();
    }

    /// @brief Take `other`'s words, leaving it of no bits.
    /// @pre `other`'s words are its own allocation, not a holder's buffer.
    DenseGF2Vector& operator=(DenseGF2Vector&& other) noexcept {
        assert(other.owns_words_ && "a vector is moved out of its holder's buffer");
        if (this != &other) {
            if (owns_words_) {
                delete[] words_;
            }
            num_bits_ = other.num_bits_;
            capacity_ = other.capacity_;
            words_ = other.words_;
            owns_words_ = true;
            other.release();
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

    /// @pre `i < num_bits()`.
    void flip_bit(std::size_t i) noexcept {
        assert(i < num_bits_ && "flip_bit index is out of range");
        words_[i >> word_shift] ^= Word{1} << (i & word_mask);
    }

    /// @brief Get index of the first set bit, or `std::nullopt` if none are set.
    std::optional<std::size_t> leading_bit() const noexcept {
        return leading_bit(words_, word_count(num_bits_));
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
        copy_words(words_, other.words_, word_count(num_bits_));
    }

    /// @brief Overwrite from `num_words()` words packed as `words()` is.
    /// @pre The bits of `words` past `num_bits()` are zero. Not asserted.
    void assign_from_words(const Word* words) noexcept {
        copy_words(words_, words, word_count(num_bits_));
    }

    bool equals(const DenseGF2Vector& other) const noexcept {
        return num_bits_ == other.num_bits_ &&
               std::memcmp(words_, other.words_, num_words() * sizeof(Word)) == 0;
    }

    /// @pre `other.num_bits() == num_bits() >= 1`.
    bool equals_except_last_bit(const DenseGF2Vector& other) const noexcept {
        assert(num_bits_ != 0 && num_bits_ == other.num_bits_ &&
               "bit count mismatch in equals_except_last_bit");
        const std::size_t last = num_words() - 1;
        const Word differing = words_[last] ^ other.words_[last];
        return (differing & ~(Word{1} << ((num_bits_ - 1) & word_mask))) == 0 &&
               std::memcmp(words_, other.words_, last * sizeof(Word)) == 0;
    }

    std::size_t hash() const noexcept;

    /// @brief A hash that `equals_except_last_bit()` vectors share.
    /// @pre `num_bits() >= 1`.
    std::size_t hash_except_last_bit() const noexcept;

    /// @brief Call `f(bit)` for each set bit, in ascending order.
    /// @pre `f` does not change this vector.
    template <class F>
    void for_each_set_bit(F&& f) const {
        const Word* words = words_;
        const std::size_t num_words = word_count(num_bits_);
        std::size_t w = 0;
        while (true) {
            // Zero words four at a time, then one: most words of a sparse row are zero.
            while (w + 4 <= num_words && (words[w] | words[w + 1] | words[w + 2] | words[w + 3]) == 0) {
                w += 4;
            }
            while (w < num_words && words[w] == 0) {
                ++w;
            }
            if (w == num_words) {
                return;
            }
            for (Word word = words[w]; word != 0; word &= word - 1) {
                f((w << word_shift) + static_cast<std::size_t>(std::countr_zero(word)));
            }
            ++w;
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
    /// @brief Become a vector of no bits and no words, which can be moved or assigned again.
    void release() noexcept {
        num_bits_ = 0;
        capacity_ = 0;
        words_ = nullptr;
        owns_words_ = true;
    }

#ifndef NDEBUG
    /// @brief Whether every bit below `i` is zero, for the fused add's precondition.
    bool no_set_bit_below(std::size_t i) const noexcept {
        const std::optional<std::size_t> first = leading_bit();
        return !first || *first >= i;
    }
#endif

    std::size_t num_bits_;
    std::size_t capacity_; ///< In words.
    Word* words_;
    bool owns_words_; ///< False while the words are in a buffer the holder allocated.
};

} // namespace xorcle
