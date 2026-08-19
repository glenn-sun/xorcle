#pragma once

#include <concepts>
#include <cstddef>
#include <optional>
#include <span>

#include "algebra/equation_repr.h"
#include "algebra/reduce_result.h"

namespace xorcle {

/// @brief The storage contract every equation representation implements.
///
/// A `GF2Vector` is a concrete, non-virtual, fully inlinable vector over GF(2)
/// of @f$w = n + 1@f$ bits @f$[0, w)@f$, where bit @f$n@f$ is the constant bit.
/// The row-echelon algorithms are written once, as templates over this concept,
/// so that every inner loop inlines. Per-bit operations (`test_bit`, `set_bit`,
/// `next_bit`, `for_each_bit`) are used freely by those templates because they
/// are direct calls, not dispatch; they are deliberately **absent** from the
/// public `Equation` and `RowEchelonMatrix`, where the same operations would be
/// virtual and cost orders of magnitude more per bit.
///
/// @note `classify()` must be implemented directly by each representation -- a
/// word scan for dense, a size-and-front check for sparse -- and never composed
/// from separate emptiness and constant-bit queries.
template <class V>
concept GF2Vector = requires(V v, const V cv, std::size_t i, std::span<const std::size_t> bits) {
    // Lets the templated algorithms assert that a downcast from the abstract
    // interface is well-founded.
    { V::repr } -> std::convertible_to<EquationRepr>;

    { V(i) }; // zero vector of `i` bits
    { cv.num_bits() } -> std::same_as<std::size_t>;
    { v.resize(i) } -> std::same_as<void>;
    { v.clear() } -> std::same_as<void>; // set to all-zero, bit count unchanged
    { v.assign_bits(bits) } -> std::same_as<void>; // clear, then set each listed bit
    { cv.test_bit(i) } -> std::same_as<bool>;
    { v.set_bit(i) } -> std::same_as<void>;
    { cv.leading_bit() } -> std::same_as<std::optional<std::size_t>>;
    { cv.next_bit(i) } -> std::same_as<std::optional<std::size_t>>; // strictly after `i`
    { cv.is_zero() } -> std::same_as<bool>;
    { cv.classify() } -> std::same_as<ReduceResult>;
    { v.plus_assign(cv) } -> std::same_as<void>; // bit counts must match
    { v.assign_from(cv) } -> std::same_as<void>; // bit counts must match, no allocation
    { cv.equals(cv) } -> std::same_as<bool>;
    { cv.hash() } -> std::same_as<std::size_t>;
    { cv.for_each_bit([](std::size_t) {}) } -> std::same_as<void>; // ascending
};

} // namespace xorcle
