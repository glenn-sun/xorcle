#pragma once

#include <optional>
#include <string_view>

namespace xorcle {

/// @brief Which storage representation the algebra layer uses for a row.
///
/// Selected at runtime through the normal option system (`--algebra=`), like
/// every other solver choice. The two backends are behaviourally identical;
/// only their performance characteristics differ.
enum class Algebra {
    dense, ///< Bit-packed rows, on `gf2::BitVector`.
    sparse ///< Sorted vector of set column indices.
};

/// @brief Return the option-table spelling of a backend.
/// @param backend The backend to name.
/// @return `"dense"` or `"sparse"`.
std::string_view to_string(Algebra backend) noexcept;

/// @brief Parse a backend from its option-table spelling.
/// @param name Candidate backend name.
/// @return The backend, or `std::nullopt` if `name` is not a backend name.
std::optional<Algebra> algebra_from_string(std::string_view name) noexcept;

} // namespace xorcle
