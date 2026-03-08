#pragma once

#include <cstddef>
#include <cstdint>
#include <random>

#include "bit/vector.h"

namespace util {

using Seed = std::uint64_t;
using Rng = std::mt19937_64;
inline constexpr Seed default_seed = 0;

/// @brief Stable RNG stream identifiers derived from the CLI seed.
///
/// @note These constants are not arbitrary. They are borrowed from standard
/// fixed bit-mixing / initialization constants:
/// - the first three values are SHA-512 initial hash words
/// - the last two values are additional SHA-512 initial hash words
/// They are used only as distinct, fixed component tags when deriving
/// independent deterministic sub-seeds.
enum class RngComponent : std::uint64_t {
    decision_phase = 0x6a09e667f3bcc909ULL,
    random_clause_order = 0xbb67ae8584caa73bULL,
    random_clause_equation = 0x3c6ef372fe94f82bULL,
    vsids_clause_start = 0xa54ff53a5f1d36f1ULL,
    cvsids_clause_equation = 0x510e527fade682d1ULL
};

/// @brief Derive a stable sub-seed from a base solver seed.
/// @param base_seed User-visible seed shared across the solver.
/// @param component Logical RNG stream identifier.
/// @param salt Optional extra distinguisher for future callers.
/// @return Stable derived seed for the requested component stream.
/// @note The derivation uses SplitMix64-style mixing so that nearby seeds and
/// component tags produce decorrelated sub-seeds.
Seed derive_seed(Seed base_seed, RngComponent component, std::uint64_t salt = 0);

/// @brief Construct a deterministic RNG stream from the base solver seed.
/// @param base_seed User-visible seed shared across the solver.
/// @param component Logical RNG stream identifier.
/// @param salt Optional extra distinguisher for future callers.
/// @return Seeded RNG for the requested component stream.
Rng make_rng(Seed base_seed, RngComponent component, std::uint64_t salt = 0);

/// @brief Return a uniform random index in the range [0, upper_bound - 1].
/// @param rng RNG stream to draw from.
/// @param upper_bound Exclusive upper bound.
/// @return A uniformly sampled index.
/// @pre `upper_bound > 0`.
std::size_t random_index(Rng& rng, std::size_t upper_bound);

/// @brief Return a uniform random integer in the range [0, upper_bound - 1].
/// @param rng RNG stream to draw from.
/// @param upper_bound Exclusive upper bound.
/// @return A uniformly sampled integer.
/// @pre `upper_bound > 0`.
int random_int(Rng& rng, int upper_bound);

/// @brief Sample a random non-zero GF(2) coefficient vector.
/// @param num_bits Number of coefficients to sample.
/// @param rng RNG stream to draw from.
/// @pre `num_bits > 0`.
/// @post The returned vector has size `num_bits` and is non-zero.
bit::vector<> random_nonzero_coefficients(std::size_t num_bits, Rng& rng);

} // namespace util
