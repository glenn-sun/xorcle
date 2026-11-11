#include "util/random.h"

#include <algorithm>
#include <bit>
#include <cassert>

namespace xorcle {
namespace {

/// @brief Bits consumed from the RNG per draw.
constexpr std::size_t bits_per_draw = 64;

/// @brief SplitMix64 finalizer, with the gamma increment applied first.
Seed mix_seed(Seed x) {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

} // namespace

Seed
derive_seed(Seed base_seed, RngComponent component) {
    const Seed component_seed = static_cast<Seed>(component);
    return mix_seed(mix_seed(base_seed) ^ mix_seed(component_seed) ^ mix_seed(0));
}

Rng
make_rng(Seed base_seed, RngComponent component) {
    return Rng(derive_seed(base_seed, component));
}

std::size_t
uniform_below(Rng& rng, std::size_t bound) {
    assert(bound > 0 && "a draw needs something to draw from");
    if (bound == 1) {
        return 0;
    }
    const std::uint64_t mask = std::bit_ceil(static_cast<std::uint64_t>(bound)) - 1;
    while (true) {
        const std::uint64_t draw = rng() & mask;
        if (draw < bound) {
            return static_cast<std::size_t>(draw);
        }
    }
}

IndexSet
random_nonzero_coefficients(std::size_t num_rows, Rng& rng) {
    assert(num_rows > 0 && "random_nonzero_coefficients needs a positive row count");

    const std::size_t draws_per_attempt = (num_rows + bits_per_draw - 1) / bits_per_draw;

    IndexSet coefficients(num_rows);
    do {
        coefficients.clear();
        for (std::size_t draw = 0; draw < draws_per_attempt; ++draw) {
            const std::uint64_t word = rng();
            const std::size_t first_row_idx = draw * bits_per_draw;
            const std::size_t indices_this_draw = std::min(bits_per_draw, num_rows - first_row_idx);
            for (std::size_t offset = 0; offset < indices_this_draw; ++offset) {
                if ((word >> offset) & 1ULL) {
                    coefficients.set(first_row_idx + offset);
                }
            }
        }
    } while (coefficients.none());

    return coefficients;
}

} // namespace xorcle
