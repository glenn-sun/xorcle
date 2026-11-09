#pragma once

#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace xorcle {

/// @brief A bit-packed set of indices drawn from `[0, size())`, whose index
/// domain -- columns, matrix rows or variables -- is documented by each holder.
class IndexSet {
public:
    explicit IndexSet(std::size_t size = 0) : size_(size), words_(word_count(size), 0) {}

    std::size_t size() const noexcept { return size_; }

    /// @brief Remove every index, leaving the size unchanged.
    void clear() noexcept {
        for (std::uint64_t& word : words_) {
            word = 0;
        }
    }

    /// @brief Draw from `[0, size)` instead, removing every index and keeping
    /// whatever storage is already held.
    void resize(std::size_t size) {
        size_ = size;
        words_.assign(word_count(size), 0);
    }

    /// @brief Add every index of `other`.
    /// @pre `other.size() == size()`.
    void or_with(const IndexSet& other) noexcept {
        assert(other.size_ == size_ && "IndexSet::or_with size mismatch");
        for (std::size_t w = 0; w < words_.size(); ++w) {
            words_[w] |= other.words_[w];
        }
    }

    /// @brief Add every index whose bit is set in `words`, packed as `word()` reads them.
    /// @pre `words` has a word per 64 indices of `size()`, with no bit set past it.
    void or_with_words(std::span<const std::uint64_t> words) noexcept {
        assert(words.size() == words_.size() && "IndexSet::or_with_words word count mismatch");
        std::uint64_t* const own_words = words_.data();
        for (std::size_t w = 0; w < words.size(); ++w) {
            own_words[w] |= words[w];
        }
    }

    /// @pre `i < size()`, here and in `set()` and `reset()`.
    bool test(std::size_t i) const noexcept {
        assert(i < size_ && "IndexSet::test index is out of range");
        return (words_[i >> 6] >> (i & 63)) & 1u;
    }

    void set(std::size_t i) noexcept {
        assert(i < size_ && "IndexSet::set index is out of range");
        words_[i >> 6] |= std::uint64_t{1} << (i & 63);
    }

    void reset(std::size_t i) noexcept {
        assert(i < size_ && "IndexSet::reset index is out of range");
        words_[i >> 6] &= ~(std::uint64_t{1} << (i & 63));
    }

    /// @brief Number of set indices strictly below `i`.
    /// @pre `i <= size()`.
    std::size_t count_below(std::size_t i) const noexcept;

    /// @brief Number of set indices in `[begin, end)`.
    /// @pre `begin <= end <= size()`.
    std::size_t count_between(std::size_t begin, std::size_t end) const noexcept {
        assert(begin <= end && end <= size_ && "IndexSet::count_between range is out of range");
        if (begin == end) {
            return 0;
        }
        const std::uint64_t* const words = words_.data();
        const std::size_t first_word = begin >> 6;
        const std::size_t last_word = (end - 1) >> 6;
        const std::uint64_t first_mask = ~std::uint64_t{0} << (begin & 63);
        const std::uint64_t last_mask = ~std::uint64_t{0} >> (63 - ((end - 1) & 63));
        if (first_word == last_word) {
            return static_cast<std::size_t>(std::popcount(words[first_word] & first_mask & last_mask));
        }
        auto total = static_cast<std::size_t>(std::popcount(words[first_word] & first_mask));
        for (std::size_t w = first_word + 1; w < last_word; ++w) {
            total += static_cast<std::size_t>(std::popcount(words[w]));
        }
        return total + static_cast<std::size_t>(std::popcount(words[last_word] & last_mask));
    }

    std::size_t count() const noexcept;

    /// @brief The word holding indices `[64 w, 64 w + 64)`, lowest index in the
    /// lowest bit; indices past `size()` are zero.
    /// @pre `w < (size() + 63) / 64`.
    std::uint64_t word(std::size_t w) const noexcept {
        assert(w < words_.size() && "IndexSet::word index is out of range");
        return words_[w];
    }

    bool none() const noexcept;

    /// @pre `other.size() == size()`.
    bool intersects(const IndexSet& other) const noexcept;

    /// @pre `other.size() == size()`.
    bool is_subset_of(const IndexSet& other) const noexcept;

    /// @brief Call `f(index)` for each set index, in ascending order.
    template <class F>
    void for_each_set(F&& f) const {
        const std::uint64_t* const words = words_.data();
        const std::size_t num_words = words_.size();
        std::size_t w = 0;
        while (w < num_words) {
            // Most sets are sparse, so four empty words are passed with one test.
            if ((w & 3) == 0 && w + 4 <= num_words &&
                (words[w] | words[w + 1] | words[w + 2] | words[w + 3]) == 0) {
                w += 4;
                continue;
            }
            for (std::uint64_t word = words[w]; word != 0; word &= word - 1) {
                f((w << 6) + static_cast<std::size_t>(std::countr_zero(word)));
            }
            ++w;
        }
    }

    bool operator==(const IndexSet& other) const noexcept = default;

private:
    static std::size_t word_count(std::size_t size) noexcept { return (size + 63) / 64; }

    std::size_t size_;
    std::vector<std::uint64_t> words_;
};

} // namespace xorcle
