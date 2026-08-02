#include "util/bitmap.h"

#include <bit>

namespace xorcle {

std::size_t Bitmap::count_below(std::size_t i) const noexcept {
    const std::size_t full_words = i >> 6;
    std::size_t total = 0;
    for (std::size_t w = 0; w < full_words; ++w) {
        total += static_cast<std::size_t>(std::popcount(words_[w]));
    }
    const std::size_t remainder = i & 63;
    if (remainder != 0) {
        const std::uint64_t mask = (std::uint64_t{1} << remainder) - 1;
        total += static_cast<std::size_t>(std::popcount(words_[full_words] & mask));
    }
    return total;
}

std::size_t Bitmap::count() const noexcept {
    std::size_t total = 0;
    for (const std::uint64_t word : words_) {
        total += static_cast<std::size_t>(std::popcount(word));
    }
    return total;
}

bool Bitmap::none() const noexcept {
    for (const std::uint64_t word : words_) {
        if (word != 0) {
            return false;
        }
    }
    return true;
}

} // namespace xorcle
