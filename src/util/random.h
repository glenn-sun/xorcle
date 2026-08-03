#pragma once

/// @file random.h
/// @brief Seeded per-component RNG streams.
///
/// One user-visible seed drives every randomized decision in the solver. Each
/// component that needs randomness derives its own independent stream from that
/// seed, so adding a consumer never perturbs an existing one.
///
/// @warning This derivation is a **reproducibility contract**. The differential
/// harness requires the 0.2 binary to produce
/// identical conflict counts to the 0.1 binary given matching seeds and
/// compaction off, so every constant, every mixing step, and — for
/// `random_nonzero_coefficients` — the exact number and order of `Rng` draws is
/// fixed. `tests/unit/test_random.cpp` pins all three against values dumped
/// from the 0.1 binary.

#include <cstddef>
#include <cstdint>
#include <random>

#include "util/bitmap.h"

namespace xorcle {

/// @brief The user-visible solver seed.
using Seed = std::uint64_t;

/// @brief The generator behind every solver RNG stream.
///
/// @note `std::uniform_int_distribution` is implementation-defined, so the old
/// and new binaries must be built against the same standard library for the
/// differential harness's identical-conflict-count requirement to hold on
/// `--phase=random` and `--decisions=csids` runs.
using Rng = std::mt19937_64;

/// @brief The seed used when the CLI does not supply one.
inline constexpr Seed default_seed = 0;

/// @brief Stable RNG stream identifiers derived from the CLI seed.
///
/// @note These constants are not arbitrary. They are borrowed from SHA-512
/// initial hash words and are used only as distinct, fixed component tags when
/// deriving independent deterministic sub-seeds. The tag values are part of the
/// reproducibility contract and must not change even if the enumerator is
/// renamed — `csids_equation` was spelled `clause_vsids_equation` in 0.1 and
/// carries the same number.
enum class RngComponent : std::uint64_t {
    decision_phase = 0x6a09e667f3bcc909ULL,  ///< Random phase selection.
    csids_equation = 0xbb67ae8584caa73bULL   ///< CSIDS equation sampling within a clause.
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

/// @brief Return a uniform random integer in the range [0, upper_bound - 1].
/// @param rng RNG stream to draw from.
/// @param upper_bound Exclusive upper bound.
/// @return A uniformly sampled integer.
/// @pre `upper_bound > 0`.
int random_int(Rng& rng, int upper_bound);

/// @brief Sample a random non-zero GF(2) coefficient vector.
/// @param num_bits Number of coefficients to sample.
/// @param rng RNG stream to draw from.
/// @return A `Bitmap` of width `num_bits` with at least one bit set.
/// @pre `num_bits > 0`.
///
/// Rejection sampling: draw a full vector, retry while it is zero.
///
/// @warning The draw pattern is fixed by the reproducibility contract. Each
/// attempt consumes exactly `ceil(num_bits / 64)` calls to `rng()`, filling
/// bits in ascending index order, with indices at or beyond `num_bits`
/// discarded from the final draw. Drawing per bit, drawing 32-bit words, or
/// masking differently produces a different search from the same seed.
///
/// @note The result is a `Bitmap` — a plain container — rather than anything
/// from the algebra layer, and the fill is written over bit *indices* rather
/// than over storage words. That is deliberate: it is what makes the sparse and
/// dense backends consume the stream identically (reaching into row storage
/// from outside `algebra/dense/` is forbidden anyway).
Bitmap random_nonzero_coefficients(std::size_t num_bits, Rng& rng);

} // namespace xorcle
