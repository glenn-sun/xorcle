#pragma once

/// @file random.h
/// @brief Seeded per-component RNG streams. One user-visible seed drives every
/// randomized decision; each component derives its own independent stream from
/// it, so adding a consumer never perturbs an existing one.
///
/// @warning This derivation is a **reproducibility contract**: a given seed must
/// always produce the same search, so every constant and every mixing step is
/// fixed and must not be altered. `tests/unit/test_random.cpp` pins them.

#include <cstddef>
#include <cstdint>
#include <random>

#include "util/index_set.h"

namespace xorcle {

using Seed = std::uint64_t;

/// @note `std::uniform_int_distribution` is implementation-defined, so two
/// binaries only reproduce each other's `--phase=random` and `--decisions=csids`
/// runs if they are built against the same standard library.
using Rng = std::mt19937_64;

inline constexpr Seed default_seed = 0;

/// @brief Stable RNG stream identifiers. The constants are SHA-512 initial hash
/// words used only as distinct, fixed component tags; they are part of the
/// reproducibility contract and must not change even if an enumerator is renamed.
enum class RngComponent : std::uint64_t {
    decision_phase = 0x6a09e667f3bcc909ULL,  ///< Random phase selection.
    csids_equation = 0xbb67ae8584caa73bULL   ///< CSIDS equation sampling within a clause.
};

/// @brief Derive a stable sub-seed, `salt` being an optional extra
/// distinguisher. SplitMix64-style mixing, so that nearby seeds and component
/// tags produce decorrelated sub-seeds.
Seed derive_seed(Seed base_seed, RngComponent component, std::uint64_t salt = 0);

Rng make_rng(Seed base_seed, RngComponent component, std::uint64_t salt = 0);

/// @brief A uniform random integer in [0, upper_bound - 1].
int random_int(Rng& rng, int upper_bound);

/// @brief Sample a random non-zero GF(2) coefficient vector of `num_bits` bits,
/// indexed by clause row, by rejection sampling: draw a full vector, retry while
/// it is zero.
///
/// @warning The draw pattern is fixed by the reproducibility contract. Each
/// attempt consumes exactly `ceil(num_bits / 64)` calls to `rng()`, filling bits
/// in ascending index order, with indices at or beyond `num_bits` discarded from
/// the final draw. Drawing per bit, drawing 32-bit words, or masking differently
/// produces a different search from the same seed. Writing the fill over bit
/// *indices* rather than storage words is what makes the sparse and dense
/// representations consume the stream identically.
IndexSet random_nonzero_coefficients(std::size_t num_bits, Rng& rng);

} // namespace xorcle
