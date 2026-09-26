#pragma once

namespace xorcle {

/// @brief Which representation equations use, affecting performance but not
/// behaviour; selected at runtime via `--equation-repr=`.
enum class EquationRepr {
    dense,
    sparse
};

} // namespace xorcle
