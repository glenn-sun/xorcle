#pragma once

#include <cstddef>

namespace xorcle {

/// @brief The outcome of classifying or reducing an equation.
///
/// An equation over @f$n@f$ variables is either the true equation @f$\mathbf{0}
/// = [0 = 0]@f$, the false equation @f$\mathbf{1} = [0 = 1]@f$, or a real
/// equation with a leading column.
///
/// @note This replaces the old tree's `std::size_t` sentinel convention, where
/// `npos` meant @f$\mathbf{0}@f$ and `num_vars` meant @f$\mathbf{1}@f$. That
/// convention lived in a doc comment and was hand-checked at eight-plus call
/// sites. Do not reintroduce it, not even internally.
struct ReduceResult {
    /// @brief Which of the three cases an equation falls into.
    enum class Kind {
        eq_true, ///< @f$\mathbf{0}@f$: no column is set.
        eq_false, ///< @f$\mathbf{1}@f$: only the constant column is set.
        leading ///< A real equation; `column` is its leading column.
    };

    Kind kind; ///< The case this result represents.
    std::size_t column; ///< Leading column; meaningful only when `kind == Kind::leading`.

    /// @brief The true equation @f$\mathbf{0}@f$.
    static constexpr ReduceResult eq_true() noexcept { return {Kind::eq_true, 0}; }

    /// @brief The false equation @f$\mathbf{1}@f$.
    static constexpr ReduceResult eq_false() noexcept { return {Kind::eq_false, 0}; }

    /// @brief A real equation with the given leading column.
    /// @param column The leading (first set) column.
    static constexpr ReduceResult leading(std::size_t column) noexcept {
        return {Kind::leading, column};
    }

    /// @brief Whether this result carries a leading column.
    constexpr bool is_leading() const noexcept { return kind == Kind::leading; }

    /// @brief Compare two results. Ignores `column` unless both are `leading`.
    constexpr bool operator==(const ReduceResult& other) const noexcept {
        return kind == other.kind && (kind != Kind::leading || column == other.column);
    }
};

} // namespace xorcle
