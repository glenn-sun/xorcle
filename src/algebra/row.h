#pragma once

#include <concepts>
#include <cstddef>
#include <optional>
#include <span>

#include "algebra/algebra.h"
#include "algebra/reduce_result.h"

namespace xorcle {

/// @brief The storage contract every algebra backend implements.
///
/// A `Row` is a concrete, non-virtual, fully inlinable row of width
/// @f$w = n + 1@f$ over columns @f$[0, w)@f$, where column @f$n@f$ is the
/// constant column.
///
/// The row-echelon algorithms are written once, as templates over this concept,
/// so that every inner loop inlines. Per-bit operations (`test`, `set`,
/// `next_column`, `for_each_column`) are used freely by those templates because
/// they are direct calls, not dispatch. They are deliberately **absent** from
/// the public `Equation` and `RowEchelonMatrix`, where the same operations
/// would be virtual and cost 3x-1200x.
///
/// @note `classify()` must be implemented directly by each backend -- a word
/// scan for dense, a size-and-front check for sparse -- and never composed from
/// separate emptiness and constant-column queries.
template <class R>
concept Row = requires(R r, const R cr, std::size_t i, std::span<const std::size_t> cols) {
    /// The backend this row belongs to. Lets the templated algorithms assert
    /// that a downcast from the abstract storage is well-founded.
    { R::algebra } -> std::convertible_to<Algebra>;

    { R(i) }; // zero row of width `i`
    { cr.width() } -> std::same_as<std::size_t>;
    { r.resize(i) } -> std::same_as<void>;
    { r.clear() } -> std::same_as<void>; // set to all-zero, width unchanged
    { r.assign_columns(cols) } -> std::same_as<void>; // clear, then set each listed column
    { cr.test(i) } -> std::same_as<bool>;
    { r.set(i) } -> std::same_as<void>;
    { cr.leading_column() } -> std::same_as<std::optional<std::size_t>>;
    { cr.next_column(i) } -> std::same_as<std::optional<std::size_t>>; // strictly after `i`
    { cr.is_zero() } -> std::same_as<bool>;
    { cr.classify() } -> std::same_as<ReduceResult>;
    { r.xor_assign(cr) } -> std::same_as<void>; // widths must match
    { r.assign_from(cr) } -> std::same_as<void>; // widths must match, no allocation
    { cr.equals(cr) } -> std::same_as<bool>;
    { cr.hash() } -> std::same_as<std::size_t>;
    { cr.for_each_column([](std::size_t) {}) } -> std::same_as<void>; // ascending
};

} // namespace xorcle
