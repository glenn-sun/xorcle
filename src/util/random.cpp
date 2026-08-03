#include "util/random.h"

#include <cassert>
#include <algorithm>

namespace xorcle {
namespace {

/// @brief Bits consumed from the RNG per draw.
///
/// Not `std::numeric_limits<...>::digits` of some storage word: the value is
/// part of the reproducibility contract, not a property of how `Bitmap` happens
/// to be laid out. `Rng::result_type` is 64 bits wide and the 0.1 tree packed
/// its coefficient vectors into 64-bit blocks, so one draw fills 64 bits.
constexpr std::size_t bits_per_draw = 64;

/// @brief SplitMix64 finalizer, with the gamma increment applied first.
///
/// Constants from Vigna's SplitMix64 generator.
Seed mix_seed(Seed x) {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

} // namespace

Seed
derive_seed(Seed base_seed, RngComponent component, std::uint64_t salt) {
    const Seed component_seed = static_cast<Seed>(component);
    return mix_seed(mix_seed(base_seed) ^ mix_seed(component_seed) ^ mix_seed(salt));
}

Rng
make_rng(Seed base_seed, RngComponent component, std::uint64_t salt) {
    return Rng(derive_seed(base_seed, component, salt));
}

int
random_int(Rng& rng, int upper_bound) {
    assert(upper_bound > 0 && "random_int needs a positive upper bound");
    std::uniform_int_distribution<int> dist(0, upper_bound - 1);
    return dist(rng);
}

Bitmap
random_nonzero_coefficients(std::size_t num_bits, Rng& rng) {
    assert(num_bits > 0 && "random_nonzero_coefficients needs a positive width");

    const std::size_t draws_per_attempt = (num_bits + bits_per_draw - 1) / bits_per_draw;

    Bitmap coefficients(num_bits);
    do {
        coefficients.clear();
        for (std::size_t draw = 0; draw < draws_per_attempt; ++draw) {
            const std::uint64_t word = rng();
            const std::size_t first_bit = draw * bits_per_draw;
            // Bits at or beyond `num_bits` are dropped. This is the 0.1 tree's
            // `blocks.back() &= tail_mask`, expressed as a bound on the index so
            // that it does not depend on any storage layout.
            const std::size_t width = std::min(bits_per_draw, num_bits - first_bit);
            for (std::size_t offset = 0; offset < width; ++offset) {
                if ((word >> offset) & 1ULL) {
                    coefficients.set(first_bit + offset);
                }
            }
        }
    } while (coefficients.none());

    return coefficients;
}

} // namespace xorcle
