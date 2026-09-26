#pragma once

#include <concepts>
#include <cstddef>
#include <optional>
#include <span>

#include "algebra/equation_repr.h"
#include "util/index_set.h"

namespace xorcle {

/// @brief Required features of various implementations of vectors over GF(2).
template <class V>
concept GF2Vector = requires(V v, const V cv, std::size_t i, std::span<const std::size_t> bits,
                            const IndexSet& indices) {
    { V::repr } -> std::convertible_to<EquationRepr>;
    { V(i) }; // zero vector of `i` bits
    { cv.num_bits() } -> std::same_as<std::size_t>;
    { v.resize(i) } -> std::same_as<void>;
    { v.set_zero() } -> std::same_as<void>;
    { v.assign_bits(bits) } -> std::same_as<void>; // set listed bits to 1 and others to 0
    { cv.get_bit(i) } -> std::same_as<bool>;
    { v.set_bit(i) } -> std::same_as<void>;
    { v.reset_bit(i) } -> std::same_as<void>;
    { cv.leading_bit() } -> std::same_as<std::optional<std::size_t>>;
    { cv.next_bit(i) } -> std::same_as<std::optional<std::size_t>>; // strictly after `i`
    { cv.first_bit_in(indices, i) } -> std::same_as<std::optional<std::size_t>>; // at or after `i`
    { v.plus_assign(cv) } -> std::same_as<void>;
    { v.plus_assign_and_next_bit(cv, i) } -> std::same_as<std::optional<std::size_t>>;
    { v.assign_from(cv) } -> std::same_as<void>; // reuses the buffer
    { cv.equals(cv) } -> std::same_as<bool>;
    { cv.hash() } -> std::same_as<std::size_t>;
    { cv.for_each_set_bit([](std::size_t) {}) } -> std::same_as<void>;
};

} // namespace xorcle
