#include "algebra/dense/dense_row.h"

#include <cstdint>
#include <cstring>

namespace xorcle {

namespace {

/// @brief Above this many words, `std::memcpy` beats a plain word loop.
///
/// Measured on Apple M2, Apple clang 17, `-O3 -DNDEBUG`, with 64 distinct
/// source/destination pairs per width so per-call overhead is amortized as it is
/// at a real `assign_from()` call site. Widths are `num_vars + 1` bits, so the
/// last word is always partially used -- the case that actually occurs.
/// Nanoseconds per copy, best of nine runs, reproduced across two invocations:
///
///     words   n_vars   gf2::copy    memcpy    copy/memcpy
///         1        0       1.274     2.112       0.60
///         2       64       2.119     3.025       0.70
///         3      128       3.587     3.270       1.10
///         4      192       3.297     3.586       0.92
///         5      256       3.580     3.857       0.93
///         6      320       3.903     4.180       0.93
///         7      384       4.107     4.478       0.92
///         8      448       4.498     2.054       2.19
///        16      960       6.825     2.734       2.50
///        32     1984      11.857     4.456       2.66
///        64     4032      22.271     7.258       3.07
///       128     8128      51.409    16.453       3.12
///       231    14720      89.832    45.117       1.99
///       250    15936      92.994    46.083       2.02
///
/// The crossover is sharp and sits between 7 and 8 words. Below it the word loop
/// wins by up to 1.7x; above it memcpy wins by 2-3x. A threshold of 4 would give
/// up 7-8% over the 4-7 word range.
///
/// The `gf2::copy` column is measured for comparison: its
/// `dst.set_word(i, src.word(i))` loop calls `clean()` on the last word, a
/// read-modify-write firing whenever `size() % 64 != 0` whose presence blocks
/// vectorization of the whole loop. The word loop below is that loop minus the
/// clean, and crosses over against memcpy at the same width. Over word-aligned
/// widths `gf2::copy` instead matches memcpy at every size (24.1 vs 24.9 ns at
/// 250 words), so the penalty is specific to ragged widths -- and equations are
/// ragged essentially always. There is still no case for padding them.
constexpr std::size_t assign_from_word_threshold = 7;

/// @brief Fibonacci hashing multiplier.
constexpr std::uint64_t hash_multiplier = 11400714819323198485ull;

} // namespace

void DenseRow::assign_from(const DenseRow& other) noexcept {
    assert(bits_.size() == other.bits_.size() && "width mismatch in assign_from");
    const std::size_t num_words = bits_.words();
    if (num_words <= assign_from_word_threshold) {
        Word* destination = bits_.store();
        const Word* source = other.bits_.store();
        for (std::size_t i = 0; i < num_words; ++i) {
            destination[i] = source[i];
        }
    } else {
        std::memcpy(bits_.store(), other.bits_.store(), num_words * sizeof(Word));
    }
}

std::size_t DenseRow::hash() const noexcept {
    // gf2 guarantees the unused high bits of the last word are always zero, so
    // hashing whole words needs no tail masking.
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < bits_.words(); ++i) {
        value ^= bits_.word(i);
        value *= hash_multiplier;
    }
    return static_cast<std::size_t>(value);
}

} // namespace xorcle
