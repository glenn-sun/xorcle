#pragma once

#include <optional>
#include <string_view>

namespace xorcle {

/// @brief Which storage representation the algebra layer uses for a row.
/// Selected at runtime via `--algebra=`. The two backends are behaviourally
/// identical; only their performance characteristics differ.
enum class Algebra {
    dense, ///< Bit-packed rows, on `gf2::BitVector`.
    sparse ///< Sorted vector of set column indices.
};

/// @brief The option-table spelling of a backend: `"dense"` or `"sparse"`.
std::string_view to_string(Algebra backend) noexcept;

/// @brief Parse a backend name, or `std::nullopt` if it names no backend.
std::optional<Algebra> algebra_from_string(std::string_view name) noexcept;

} // namespace xorcle
