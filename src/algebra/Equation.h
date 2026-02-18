#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "PrintMode.h"
#include "bit/vector.h"

namespace algebra {

/// @brief An affine equation over GF(2).
///
/// Represented as a bit::vector whose last component is the constant
/// term. In other words, if there are 3 variables, the equation
/// x1 + x3 = 1 is represented as [1, 0, 1, 1]. Note that the false
/// equation 1 = 0 has a representation in this form: when there are
/// 3 variables, it is [0, 0, 0, 1].
class Equation : public bit::vector<> {
public:
    /// @brief Construct an empty equation.
    Equation() {}

    /// @brief Construct a zero equation with space for num_vars variables.
    Equation(std::size_t num_vars) : bit::vector<>(num_vars + 1) {}

    /// @brief Construct from an existing bit vector representation.
    Equation(const bit::vector<>& v) : bit::vector<>(v) {}

    /// @brief Format the equation as a string using the given print mode.
    std::string to_string(PrintMode mode = PrintMode()) const;
};
} // namespace algebra

namespace std {
    /// @brief Hash specialization for algebra::Equation so it can be used in hash maps.
    template<>
    struct hash<algebra::Equation> {
        /// @brief Compute a hash by mixing all bit blocks.
        std::size_t operator()(algebra::Equation const& eqn) const noexcept {
            uint64_t hash = 0;
            for (uint64_t block : eqn.blocks()) {
                hash ^= block;
                hash *= 11400714819323198485llu;
            }
            return hash;
        }
    };
}
