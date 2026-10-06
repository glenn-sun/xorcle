#include "util/index_set.h"

#include <bit>
#include <cassert>

namespace xorcle {

std::size_t IndexSet::count_below(std::size_t i) const noexcept {
    assert(i <= size_ && "IndexSet::count_below index is out of range");
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

std::size_t IndexSet::count() const noexcept {
    std::size_t total = 0;
    for (const std::uint64_t word : words_) {
        total += static_cast<std::size_t>(std::popcount(word));
    }
    return total;
}

bool IndexSet::none() const noexcept {
    for (const std::uint64_t word : words_) {
        if (word != 0) {
            return false;
        }
    }
    return true;
}

bool IndexSet::intersects(const IndexSet& other) const noexcept {
    assert(other.size_ == size_ && "IndexSet::intersects size mismatch");
    for (std::size_t w = 0; w < words_.size(); ++w) {
        if ((words_[w] & other.words_[w]) != 0) {
            return true;
        }
    }
    return false;
}

} // namespace xorcle
