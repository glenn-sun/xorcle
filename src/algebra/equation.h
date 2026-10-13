#pragma once

#include <cassert>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "algebra/compaction_transform.h"
#include "algebra/equation_repr.h"
#include "algebra/equation_template.h"
#include "algebra/gf2_vector.h"
#include "algebra/reduce_result.h"
#include "util/print_mode.h"

namespace xorcle {

/// @brief An affine equation over GF(2).
///
/// Wraps an `EquationInterface` pointer to allow value semantics.
class Equation {
public:
    /// @brief A null handle
    Equation() noexcept = default;

    /// @brief The true equation `0 = 0` over `num_vars` variables.
    Equation(EquationRepr repr, std::size_t num_vars);

    /// @brief Adopt an already-built interface, which may be null.
    explicit Equation(std::unique_ptr<EquationInterface> interface) noexcept
        : interface_(std::move(interface)) {}

    Equation(const Equation& other);
    Equation(Equation&& other) noexcept = default;
    Equation& operator=(const Equation& other);
    Equation& operator=(Equation&& other) noexcept = default;
    ~Equation() = default;

    /// @brief The all-zero vector of `num_bits` bits.
    static Equation with_num_bits(EquationRepr repr, std::size_t num_bits);

    static Equation eq_true(EquationRepr repr, std::size_t num_vars);

    static Equation eq_false(EquationRepr repr, std::size_t num_vars);

    /// @brief Set exactly `bits`, possibly including the constant bit.
    static Equation from_bits(EquationRepr repr, std::size_t num_vars,
                              std::span<const std::size_t> bits);

    /// @pre `!is_null()`.
    EquationRepr repr() const noexcept;

    /// @pre `!is_null()`.
    std::size_t num_vars() const noexcept;

    /// @brief Bit count, i.e. `num_vars() + 1`; zero for a null handle.
    std::size_t num_bits() const noexcept { return interface_ ? interface_->num_bits() : 0; }

    bool is_null() const noexcept { return interface_ == nullptr; }

    /// @brief Resize to hold `num_vars` variables, zeroing any new bits.
    /// @pre `!is_null()`.
    void resize(std::size_t num_vars);

    /// @brief Set all bits to zero, representing `0 = 0`.
    /// @pre `!is_null()`.
    void set_eq_true() noexcept;

    /// @brief Set only the constant bit, representing `0 = 1`.
    /// @pre `!is_null()`.
    void set_eq_false() noexcept;

    /// @brief Get index of the first set bit, or `std::nullopt` if none are set.
    /// @pre `!is_null()`, here and in `classify()`, `is_eq_true()` and `is_eq_false()`.
    std::optional<std::size_t> leading_bit() const noexcept {
        assert(interface_ != nullptr && "leading_bit on a null equation handle");
        return interface_->leading_bit();
    }

    ReduceResult classify() const noexcept {
        assert(interface_ != nullptr && "classify on a null equation handle");
        return interface_->classify();
    }

    bool is_eq_true() const noexcept { return classify().kind == ReduceResult::Kind::eq_true; }

    bool is_eq_false() const noexcept { return classify().kind == ReduceResult::Kind::eq_false; }

    /// @brief Add `other` into this equation over GF(2).
    /// @pre Neither handle is null, and `other` shares this equation's
    /// representation and bit count.
    void plus_assign(const Equation& other) {
        assert(interface_ != nullptr && other.interface_ != nullptr &&
               "plus_assign on a null equation handle");
        assert(interface_->repr() == other.interface_->repr() &&
               "representation mismatch in plus_assign");
        assert(interface_->num_bits() == other.interface_->num_bits() &&
               "bit count mismatch in plus_assign");
        interface_->plus_assign(*other.interface_);
    }

    /// @brief Overwrite this equation's contents from `other`, reusing its buffer.
    /// @pre Neither handle is null, and `other` shares this equation's
    /// representation and bit count.
    void assign_from(const Equation& other) {
        assert(interface_ != nullptr && other.interface_ != nullptr &&
               "assign_from on a null equation handle");
        assert(interface_->repr() == other.interface_->repr() &&
               "representation mismatch in assign_from");
        assert(interface_->num_bits() == other.interface_->num_bits() &&
               "bit count mismatch in assign_from");
        interface_->assign_from(*other.interface_);
    }

    /// @brief Overwrite this equation to have exactly the given bits set, reusing
    /// its buffer.
    /// @param bits Bits to set; need not be sorted or distinct, may include the
    /// constant bit `num_vars()`, and must each be `< num_bits()`.
    /// @pre `!is_null()`.
    void assign_bits(std::span<const std::size_t> bits);

    /// @brief Old active variable numbering to the compacted one.
    /// @pre `!is_null()` and `num_bits() == transform.old_num_vars() + 1`.
    Equation compact(const CompactionTransform& transform) const;

    /// @brief Ascending list of set bits; empty for a null handle.
    std::vector<std::size_t> bits() const;

    /// @brief Number of set bits, the constant bit included; zero for a null handle.
    std::size_t num_set_bits() const noexcept {
        return interface_ ? interface_->num_set_bits() : 0;
    }

    /// @brief Null handles compare equal to each other only.
    bool operator==(const Equation& other) const noexcept;

    std::size_t hash() const noexcept { return interface_ ? interface_->hash() : 0; }

    /// @brief Whether `other` is this equation or its negation.
    /// @pre Neither handle is null, and `other` shares this equation's
    /// representation and bit count.
    bool equal_variables(const Equation& other) const noexcept {
        assert(interface_ != nullptr && other.interface_ != nullptr &&
               "equal_variables on a null equation handle");
        assert(interface_->repr() == other.interface_->repr() &&
               "representation mismatch in equal_variables");
        assert(interface_->num_bits() == other.interface_->num_bits() &&
               "bit count mismatch in equal_variables");
        return interface_->equal_variables(*other.interface_);
    }

    /// @brief A hash an equation shares with its negation.
    /// @pre `!is_null()`.
    std::size_t variables_hash() const noexcept {
        assert(interface_ != nullptr && "variables_hash on a null equation handle");
        return interface_->variables_hash();
    }

    /// @brief Render as e.g. `[x1 + x3 = 1]`.
    std::string to_string(const PrintMode& mode = {}) const;

    /// @brief For use inside `src/algebra/` only.
    /// @pre `!is_null()`, here and in the const overload.
    EquationInterface& interface() noexcept {
        assert(interface_ != nullptr && "interface() on a null equation handle");
        return *interface_;
    }

    const EquationInterface& interface() const noexcept {
        assert(interface_ != nullptr && "interface() on a null equation handle");
        return *interface_;
    }

private:
    std::unique_ptr<EquationInterface> interface_;
};

/// @brief Hash functor so `Equation` can key a `std::unordered_map`.
struct EquationHash {
    std::size_t operator()(const Equation& equation) const noexcept { return equation.hash(); }
};

/// @brief Hash and equality functors under which an equation and its negation are
/// one key.
struct EquationVariablesHash {
    std::size_t operator()(const Equation& equation) const noexcept {
        return equation.variables_hash();
    }
};

struct EquationVariablesEqual {
    bool operator()(const Equation& a, const Equation& b) const noexcept {
        return a.equal_variables(b);
    }
};

std::unique_ptr<EquationInterface> make_equation_interface(EquationRepr repr,
                                                           std::size_t num_bits);

/// @brief Build the true equation `0 = 0` in the chosen representation.
Equation make_equation(EquationRepr repr, std::size_t num_vars);

/// @pre `!equation.is_null()` and `equation.repr() == V::repr`
template <GF2Vector V>
const V& gf2_vector_of(const Equation& equation) noexcept {
    assert(equation.repr() == V::repr && "representation mismatch in gf2_vector_of");
    return static_cast<const EquationTemplate<V>&>(equation.interface()).vector();
}

/// @pre `!equation.is_null()` and `equation.repr() == V::repr`
template <GF2Vector V>
V& gf2_vector_of(Equation& equation) noexcept {
    assert(equation.repr() == V::repr && "representation mismatch in gf2_vector_of");
    return static_cast<EquationTemplate<V>&>(equation.interface()).vector();
}

} // namespace xorcle
