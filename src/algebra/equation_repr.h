#pragma once

#include <optional>
#include <string_view>

namespace xorcle {

/// @brief Storage representation of equations, affecting performance but not
/// behaviour. Selected at runtime via `--equation-repr=`.
enum class EquationRepr {
    dense,
    sparse
};

/// @brief Option-table spelling of a representation: `"dense"` or `"sparse"`.
std::string_view to_string(EquationRepr repr) noexcept;

/// @brief Parse a representation name, or `std::nullopt` if it names none.
std::optional<EquationRepr> equation_repr_from_string(std::string_view name) noexcept;

} // namespace xorcle
