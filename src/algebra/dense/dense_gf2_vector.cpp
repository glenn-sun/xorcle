#include "algebra/dense/dense_gf2_vector.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace xorcle {

void DenseGF2Vector::resize(std::size_t num_bits) {
    const std::size_t old_num_words = num_words();
    const std::size_t new_num_words = word_count(num_bits);
    if (new_num_words > capacity_) {
        Word* words = new Word[new_num_words];
        if (old_num_words != 0) {
            std::memcpy(words, words_, old_num_words * sizeof(Word));
        }
        if (owns_words_) {
            delete[] words_;
        }
        words_ = words;
        capacity_ = new_num_words;
        owns_words_ = true;
    }
    if (new_num_words > old_num_words) {
        std::memset(words_ + old_num_words, 0, (new_num_words - old_num_words) * sizeof(Word));
    } else if (num_bits < num_bits_ && (num_bits & word_mask) != 0) {
        words_[new_num_words - 1] &= ~(~Word{0} << (num_bits & word_mask));
    }
    num_bits_ = num_bits;
}

namespace {

using Word = DenseGF2Vector::Word;

/// @brief Chain word `i` into a hash of nonzero words, each with its index, so a
/// vector with few set bits hashes at the cost of scanning its words.
inline std::uint64_t hash_word(std::uint64_t value, const std::size_t i, const Word word) noexcept {
    // Fibonacci hash
    value = (value ^ i) * 11400714819323198485ull;
    return (value ^ word) * 11400714819323198485ull;
}

} // namespace

std::size_t DenseGF2Vector::hash() const noexcept {
    const Word* words = words_;
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < num_words(); ++i) {
        if (words[i] != 0) {
            value = hash_word(value, i, words[i]);
        }
    }
    return static_cast<std::size_t>(value);
}

void DenseGF2Vector::append_key_except_last_bit(std::string& key) const {
    assert(num_bits_ != 0 && "append_key_except_last_bit on a vector of no bits");
    const Word* words = words_;
    const std::size_t last = num_words() - 1;
    const auto append = [&key](const std::size_t i, const Word word) {
        const auto idx = static_cast<std::uint32_t>(i);
        key.append(reinterpret_cast<const char*>(&idx), sizeof(idx));
        key.append(reinterpret_cast<const char*>(&word), sizeof(word));
    };
    for (std::size_t i = 0; i < last; ++i) {
        if (words[i] != 0) {
            append(i, words[i]);
        }
    }
    const Word word = words[last] & ~(Word{1} << ((num_bits_ - 1) & word_mask));
    if (word != 0) {
        append(last, word);
    }
}

std::size_t DenseGF2Vector::hash_except_last_bit() const noexcept {
    assert(num_bits_ != 0 && "hash_except_last_bit on a vector of no bits");
    const Word* words = words_;
    const std::size_t last = num_words() - 1;
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < last; ++i) {
        if (words[i] != 0) {
            value = hash_word(value, i, words[i]);
        }
    }
    const Word word = words[last] & ~(Word{1} << ((num_bits_ - 1) & word_mask));
    if (word != 0) {
        value = hash_word(value, last, word);
    }
    return static_cast<std::size_t>(value);
}

} // namespace xorcle
