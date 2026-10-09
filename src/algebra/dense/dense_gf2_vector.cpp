#include "algebra/dense/dense_gf2_vector.h"

#include <cstdint>
#include <cstring>

namespace xorcle {

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
