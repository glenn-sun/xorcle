#pragma once

#include <cstddef>

namespace xorcle {

/// @brief The outcome of classifying or reducing an equation.
///
/// An equation over @f$n@f$ variables is either the true equation @f$\mathbf{0}
/// = [0 = 0]@f$, the false equation @f$\mathbf{1} = [0 = 1]@f$, or a real
/// equation with a leading column. These three cases are always carried in this
/// type, never in a `std::size_t` sentinel a caller has to remember to decode.
struct ReduceResult {
    enum class Kind {
        eq_true, ///< @f$\mathbf{0}@f$: no column is set.
        eq_false, ///< @f$\mathbf{1}@f$: only the constant column is set.
        leading ///< A real equation; `column` is its leading column.
    };

    Kind kind;
    std::size_t column; ///< Meaningful only when `kind == Kind::leading`.

    static constexpr ReduceResult eq_true() noexcept { return {Kind::eq_true, 0}; }

    static constexpr ReduceResult eq_false() noexcept { return {Kind::eq_false, 0}; }

    static constexpr ReduceResult leading(std::size_t column) noexcept {
        return {Kind::leading, column};
    }

    constexpr bool is_leading() const noexcept { return kind == Kind::leading; }

    /// @brief Ignores `column` unless both results are `leading`.
    constexpr bool operator==(const ReduceResult& other) const noexcept {
        return kind == other.kind && (kind != Kind::leading || column == other.column);
    }
};

} // namespace xorcle
