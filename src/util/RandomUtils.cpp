#include "util/RandomUtils.h"

#include <cassert>
#include <limits>

namespace util {
namespace {

// SplitMix64 finalizer constants from Vigna's SplitMix64 generator.
Seed mix_seed(Seed x) {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

} // namespace

Seed derive_seed(Seed base_seed, RngComponent component, std::uint64_t salt) {
    const Seed component_seed = static_cast<Seed>(component);
    return mix_seed(mix_seed(base_seed) ^ mix_seed(component_seed) ^ mix_seed(salt));
}

Rng make_rng(Seed base_seed, RngComponent component, std::uint64_t salt) {
    return Rng(derive_seed(base_seed, component, salt));
}

std::size_t random_index(Rng& rng, std::size_t upper_bound) {
    assert(upper_bound > 0);
    std::uniform_int_distribution<std::size_t> dist(0, upper_bound - 1);
    return dist(rng);
}

int random_int(Rng& rng, int upper_bound) {
    assert(upper_bound > 0);
    std::uniform_int_distribution<int> dist(0, upper_bound - 1);
    return dist(rng);
}

bit::vector<> random_nonzero_coefficients(std::size_t num_bits, Rng& rng) {
    assert(num_bits > 0);

    bit::vector<> coefficients(num_bits);
    auto& blocks = coefficients.blocks();
    using Block = typename bit::vector<>::block_type;
    constexpr std::size_t bits_per_block = std::numeric_limits<Block>::digits;
    const std::size_t tail_bits = num_bits % bits_per_block;
    const Block tail_mask =
        (tail_bits == 0) ? std::numeric_limits<Block>::max() : (Block{1} << tail_bits) - 1;

    do {
        for (Block& block : blocks) {
            block = static_cast<Block>(rng());
        }
        if (!blocks.empty()) {
            blocks.back() &= tail_mask;
        }
    } while (coefficients.none());

    return coefficients;
}
} // namespace util
