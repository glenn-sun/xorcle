#include "util/random.h"

#include <cassert>
#include <algorithm>

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

IndexSet
random_nonzero_coefficients(std::size_t num_rows, Rng& rng) {
    assert(num_rows > 0 && "random_nonzero_coefficients needs a positive row count");

    const std::size_t draws_per_attempt = (num_rows + bits_per_draw - 1) / bits_per_draw;

    IndexSet coefficients(num_rows);
    do {
        coefficients.clear();
        for (std::size_t draw = 0; draw < draws_per_attempt; ++draw) {
            const std::uint64_t word = rng();
            const std::size_t first_index = draw * bits_per_draw;
            const std::size_t indices_this_draw = std::min(bits_per_draw, num_rows - first_index);
            for (std::size_t offset = 0; offset < indices_this_draw; ++offset) {
                if ((word >> offset) & 1ULL) {
                    coefficients.set(first_index + offset);
                }
            }
        }
    } while (coefficients.none());

    return coefficients;
}

} // namespace xorcle
