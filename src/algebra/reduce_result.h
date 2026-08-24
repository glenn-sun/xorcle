#pragma once

#include <cstddef>

namespace xorcle {

/// @brief The outcome of classifying or reducing an equation.
///
/// An equation is either the true equation `0 = 0`, the false equation
/// `0 = 1`, or a real equation with a leading bit.
struct ReduceResult {
    enum class Kind {
        eq_true, ///< `0 = 0`: no bit is set.
        eq_false, ///< `0 = 1`: only the constant bit is set.
        leading ///< A real equation; `bit` is its leading bit.
    };

    Kind kind;
    std::size_t bit; ///< Meaningful only when `kind == Kind::leading`.

    static constexpr ReduceResult eq_true() noexcept { return {Kind::eq_true, 0}; }

    static constexpr ReduceResult eq_false() noexcept { return {Kind::eq_false, 0}; }

    static constexpr ReduceResult leading(std::size_t bit) noexcept { return {Kind::leading, bit}; }

    constexpr bool is_leading() const noexcept { return kind == Kind::leading; }

    /// @brief Ignores `bit` unless both results are `leading`.
    constexpr bool operator==(const ReduceResult& other) const noexcept {
        return kind == other.kind && (kind != Kind::leading || bit == other.bit);
    }
};

} // namespace xorcle
