#pragma once

#include <optional>
#include <string_view>

namespace xorcle {

/// @brief Which storage representation the algebra layer uses for an equation.
/// Selected at runtime via `--equation-repr=`. The two are behaviourally identical;
/// only their performance characteristics differ.
enum class EquationRepr {
    dense, ///< Bit-packed equations, on `gf2::BitVector`.
    sparse ///< Sorted vector of set bit indices.
};

/// @brief The option-table spelling of a representation: `"dense"` or `"sparse"`.
std::string_view to_string(EquationRepr repr) noexcept;

/// @brief Parse a representation name, or `std::nullopt` if it names none.
std::optional<EquationRepr> equation_repr_from_string(std::string_view name) noexcept;

} // namespace xorcle
