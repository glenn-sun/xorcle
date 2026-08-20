#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "algebra/compaction_transform.h"
#include "algebra/equation_repr.h"
#include "algebra/reduce_result.h"
#include "util/print_mode.h"

namespace xorcle {

class Equation;

/// @brief The abstract interface behind `Equation`.
/// Contains all functionality involving per-bit loops, to avoid virtual
/// dispatch at low levels.
class EquationInterface {
public:
    virtual ~EquationInterface() = default;

    virtual EquationRepr repr() const noexcept = 0;

    virtual std::unique_ptr<EquationInterface> clone() const = 0;

    virtual std::size_t num_bits() const noexcept = 0;

    virtual void resize(std::size_t num_bits) = 0;

    virtual void set_eq_true() noexcept = 0;

    virtual void assign_bits(std::span<const std::size_t> bits) = 0;

    virtual std::optional<std::size_t> leading_bit() const noexcept = 0;

    virtual ReduceResult classify() const noexcept = 0;

    virtual void plus_assign(const EquationInterface& other) = 0;

    virtual void assign_from(const EquationInterface& other) = 0;

    virtual std::unique_ptr<EquationInterface> compact(const CompactionTransform& transform)
        const = 0;

    virtual std::unique_ptr<EquationInterface> uncompact(const CompactionTransform& transform)
        const = 0;

    virtual void bits(std::vector<std::size_t>& out) const = 0;

    virtual bool equals(const EquationInterface& other) const noexcept = 0;

    virtual std::size_t hash() const noexcept = 0;

    virtual std::string to_string(const PrintMode& mode) const = 0;
};

/// @brief An affine equation over GF(2). Wraps an `EquationInterface` pointer
/// to allow value semantics.
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

    static Equation eq_true(EquationRepr repr, std::size_t num_vars);

    static Equation eq_false(EquationRepr repr, std::size_t num_vars);

    /// @brief Set exactly `bits`, possibly including the constant bit.
    static Equation from_bits(EquationRepr repr, std::size_t num_vars,
                              std::span<const std::size_t> bits);

    EquationRepr repr() const noexcept;

    std::size_t num_vars() const noexcept;

    /// @brief Bit count, i.e. `num_vars() + 1`; zero for a null handle.
    std::size_t num_bits() const noexcept { return interface_ ? interface_->num_bits() : 0; }

    bool is_null() const noexcept { return interface_ == nullptr; }

    /// @brief Resize to hold `num_vars` variables, zeroing any new bits.
    void resize(std::size_t num_vars);

    /// @brief Set all bits to zero, representing `0 = 0`.
    void set_eq_true() noexcept;

    /// @brief Get index of the first set bit, or `std::nullopt` if none are set.
    std::optional<std::size_t> leading_bit() const noexcept { return interface_->leading_bit(); }

    ReduceResult classify() const noexcept { return interface_->classify(); }

    bool is_eq_true() const noexcept {
        return interface_->classify().kind == ReduceResult::Kind::eq_true;
    }

    bool is_eq_false() const noexcept {
        return interface_->classify().kind == ReduceResult::Kind::eq_false;
    }

    /// @brief Add `other` into this equation over GF(2).
    /// @pre `other` shares this equation's representation and bit count.
    void plus_assign(const Equation& other);

    /// @brief Overwrite this equation's contents from `other`, without
    /// allocating. Use this instead of default copy assignment for
    /// efficiency.
    /// @pre `other` shares this equation's representation and bit count.
    void assign_from(const Equation& other);

    /// @brief Overwrite this equation to have exactly the given bits set.
    /// This is the in-place counterpart of `from_bits`, which should be used
    /// instead of `from_bits` + copy, for efficiency.
    /// @param bits Bits to set; need not be sorted or distinct, may include the
    /// constant bit `num_vars()`, and must each be `< num_bits()`.
    void assign_bits(std::span<const std::size_t> bits);

    /// @brief Old active variable numbering to the compacted one.
    /// @pre `num_bits() == transform.old_num_vars() + 1`.
    Equation compact(const CompactionTransform& transform) const;

    /// @brief Compacted numbering back to the pre-compaction one.
    /// @pre `num_bits() == transform.new_num_vars() + 1`.
    Equation uncompact(const CompactionTransform& transform) const;

    /// @brief Ascending list of set bits.
    ///
    /// @note Uses an out-parameter rather than a return value because callers
    /// `TrailBasisSpace::to_RowSpace()` and `LrupLogger` refill one buffer in
    /// a loop.
    void bits(std::vector<std::size_t>& out) const;

    /// @brief Null handles compare equal to each other only.
    bool operator==(const Equation& other) const noexcept;

    std::size_t hash() const noexcept { return interface_ ? interface_->hash() : 0; }

    /// @brief Render as e.g. `[x1 + x3 = 1]`.
    std::string to_string(const PrintMode& mode = {}) const;

    /// @brief For use inside `src/algebra/` only.
    EquationInterface& interface() noexcept { return *interface_; }

    const EquationInterface& interface() const noexcept { return *interface_; }

private:
    std::unique_ptr<EquationInterface> interface_;
};

/// @brief Hash functor so `Equation` can key a `std::unordered_map`.
struct EquationHash {
    std::size_t operator()(const Equation& equation) const noexcept { return equation.hash(); }
};

std::unique_ptr<EquationInterface> make_equation_interface(EquationRepr repr,
                                                           std::size_t num_vars);

/// @brief Build the true equation `0 = 0` in the chosen representation.
Equation make_equation(EquationRepr repr, std::size_t num_vars);

} // namespace xorcle
