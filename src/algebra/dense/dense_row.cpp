#include "algebra/dense/dense_row.h"

#include <cstdint>
#include <cstring>

namespace xorcle {

namespace {

/// @brief Above this many words, `std::memcpy` beats `gf2::copy`.
///
/// Measured on the target machine (Apple M2, Apple clang 17, `-O3 -DNDEBUG`)
/// with 64 distinct source/destination pairs per width so the per-call overhead
/// is amortized the way it is at a real `assign_from()` call site. Widths are
/// `num_vars + 1` bits, i.e. the last word is always partially used -- the case
/// that actually occurs. Nanoseconds per copy, best of nine runs, reproduced
/// across two independent invocations:
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
/// The crossover is sharp and sits between 7 and 8 words, so the threshold is 7
/// rather than the plan's provisional 4 (which would have given up 7-8% over the
/// 4-7 word range). Below the threshold `gf2::copy` wins by up to 1.7x; above
/// it, memcpy wins by 2-3x.
///
/// The cause is `gf2::copy`'s `dst.set_word(i, src.word(i))` loop: `set_word`
/// calls `clean()` on the last word, and `clean()` is a read-modify-write that
/// only fires when `size() % 64 != 0`. Its presence blocks vectorization of the
/// whole loop. The same measurement over word-aligned widths shows `gf2::copy`
/// matching memcpy at every size (24.1 ns vs 24.9 ns at 250 words), which
/// confirms the plan's claim that the penalty is specific to ragged widths --
/// and equations are ragged essentially always. There is still no case for
/// padding equations to word boundaries: the penalty is specific to `copy`, and
/// `^=` and `first_set` are unaffected.
constexpr std::size_t assign_from_word_threshold = 7;

/// @brief Fibonacci hashing multiplier, as used by the old tree's equation hash.
constexpr std::uint64_t hash_multiplier = 11400714819323198485ull;

} // namespace

void DenseRow::assign_from(const DenseRow& other) noexcept {
    assert(bits_.size() == other.bits_.size() && "width mismatch in assign_from");
    if (bits_.words() <= assign_from_word_threshold) {
        gf2::copy(other.bits_, bits_);
    } else {
        std::memcpy(bits_.store(), other.bits_.store(), bits_.words() * sizeof(Word));
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
