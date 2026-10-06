#include "algebra/dense/dense_gf2_vector.h"

#include <cstdint>
#include <cstring>

namespace xorcle {

void DenseGF2Vector::assign_from(const DenseGF2Vector& other) noexcept {
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

void DenseGF2Vector::resize(std::size_t num_bits) {
    const std::size_t old_num_words = num_words();
    const std::size_t new_num_words = word_count(num_bits);
    if (new_num_words > capacity_) {
        Word* words = new Word[new_num_words];
        std::memcpy(words, words_, old_num_words * sizeof(Word));
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

std::size_t DenseGF2Vector::hash() const noexcept {
    // Only nonzero words enter the chain, each with its index, so a vector with
    // few set bits hashes at the cost of scanning its words.
    const Word* words = words_;
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < num_words(); ++i) {
        if (words[i] != 0) {
            // Fibonacci hash
            value = (value ^ i) * 11400714819323198485ull;
            value = (value ^ words[i]) * 11400714819323198485ull;
        }
    }
    return static_cast<std::size_t>(value);
}

} // namespace xorcle
