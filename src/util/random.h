#pragma once

/// @file random.h
/// @brief Seeded per-component RNG streams, each derived independently from one
/// user-visible seed.
///
/// @warning The derivation is fixed; `tests/unit/test_random.cpp` pins it.
/// Reproducing a run in another binary also needs the same standard library,
/// since `std::uniform_int_distribution` is implementation-defined.

#include <cstddef>
#include <cstdint>
#include <random>
#include <utility>
#include <vector>

#include "util/index_set.h"

namespace xorcle {

using Seed = std::uint64_t;

using Rng = std::mt19937_64;

inline constexpr Seed default_seed = 0;

/// @brief Stable RNG stream identifiers, fixed even under an enumerator rename.
enum class RngComponent : std::uint64_t {
    decision_phase = 0x6a09e667f3bcc909ULL,
    csids_equation = 0xbb67ae8584caa73bULL,
    minimize_representative = 0x3c6ef372fe94f82bULL,
    implication_graph_sample = 0xa54ff53a5f1d36f1ULL,
    vivify_basis = 0x9b05688c2b3e6c1fULL,
    bve_draw = 0x510e527fade682d1ULL,
    subsume_pass_draw = 0x1f83d9abfb41bd6bULL
};

Seed derive_seed(Seed base_seed, RngComponent component);

Rng make_rng(Seed base_seed, RngComponent component);

/// @brief A uniform random integer in [0, upper_bound - 1].
/// @pre `upper_bound > 0`.
int random_int(Rng& rng, int upper_bound);

/// @brief Sample a set of `num_rows` clause rows, redrawing until non-empty.
///
/// @warning Each draw consumes exactly `ceil(num_rows / 64)` calls to `rng()`,
/// filling indices in ascending order, with indices at or beyond `num_rows`
/// discarded.
/// @pre `num_rows > 0`.
IndexSet random_nonzero_coefficients(std::size_t num_rows, Rng& rng);

/// @brief Put @p items in a uniformly random order.
template <typename T>
void shuffle_uniformly(std::vector<T>& items, Rng& rng) {
    for (std::size_t i = items.size(); i > 1; --i) {
        std::uniform_int_distribution<std::size_t> below(0, i - 1);
        std::swap(items[i - 1], items[below(rng)]);
    }
}

} // namespace xorcle
