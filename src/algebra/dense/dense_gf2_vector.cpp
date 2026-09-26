#include "algebra/dense/dense_gf2_vector.h"

#include <cstdint>
#include <cstring>

namespace xorcle {

void DenseGF2Vector::assign_from(const DenseGF2Vector& other) noexcept {
    assert(bits_.size() == other.bits_.size() && "bit count mismatch in assign_from");
    const std::size_t num_words = bits_.words();
    // Below 8 words a plain word loop beats `std::memcpy`.
    if (num_words < 8) {
        Word* destination = bits_.store();
        const Word* source = other.bits_.store();
        for (std::size_t i = 0; i < num_words; ++i) {
            destination[i] = source[i];
        }
    } else {
        std::memcpy(bits_.store(), other.bits_.store(), num_words * sizeof(Word));
    }
}

std::size_t DenseGF2Vector::hash() const noexcept {
    // Only nonzero words enter the chain, each with its index, so a vector with
    // few set bits hashes at the cost of scanning its words.
    const Word* words = bits_.store();
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < bits_.words(); ++i) {
        if (words[i] != 0) {
            // Fibonacci hash
            value = (value ^ i) * 11400714819323198485ull;
            value = (value ^ words[i]) * 11400714819323198485ull;
        }
    }
    return static_cast<std::size_t>(value);
}

} // namespace xorcle
