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
///
/// Every method here is a whole-vector or whole-loop operation, so exactly one
/// virtual call is paid per coarse operation and none per bit. Each
/// representation derives from this through `EquationTemplate<GF2Vector>`;
/// nothing outside `src/algebra/` should name this type. The operations are
/// documented on `Equation`, which forwards.
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

/// @brief An affine equation over GF(2).
///
/// The storage width is always `num_vars + 1`: bits `[0, num_vars)` are variable
/// coefficients and bit `num_vars` is the constant bit. Over three variables
/// `x1 + x3 = 1` is `[1, 0, 1, 1]`, the false equation @f$\mathbf{1} =
/// [0 = 1]@f$ is `[0, 0, 0, 1]`, and the true equation @f$\mathbf{0} =
/// [0 = 0]@f$ is `[0, 0, 0, 0]`.
///
/// This is a handle over an `EquationInterface` chosen at construction. It is
/// deliberately missing `element`, `test`, `set`, `flip`, `blocks`, `col`, and
/// every bit iterator: exposing them would put a virtual call on each bit, which
/// costs orders of magnitude, whereas one virtual call per whole-vector
/// operation costs nothing. Algorithms needing per-bit access belong inside a
/// representation, written against the `GF2Vector` concept.
///
/// Every method other than `num_bits()`, `is_null()`, `set_eq_true()`,
/// `bits()`, `hash()`, `to_string()` and `operator==` requires `!is_null()`.
class Equation {
public:
    /// @brief A null handle: no storage, width 0.
    Equation() noexcept = default;

    /// @brief The true equation @f$\mathbf{0}@f$ over `num_vars` variables.
    Equation(EquationRepr repr, std::size_t num_vars);

    /// @brief Adopt an already-built interface, which may be null.
    explicit Equation(std::unique_ptr<EquationInterface> storage) noexcept
        : storage_(std::move(storage)) {}

    Equation(const Equation& other);
    Equation(Equation&& other) noexcept = default;
    Equation& operator=(const Equation& other);
    Equation& operator=(Equation&& other) noexcept = default;
    ~Equation() = default;

    static Equation eq_true(EquationRepr repr, std::size_t num_vars);

    static Equation eq_false(EquationRepr repr, std::size_t num_vars);

    /// @brief Set exactly `bits`, which may include the constant bit.
    static Equation from_bits(EquationRepr repr, std::size_t num_vars,
                              std::span<const std::size_t> bits);

    EquationRepr repr() const noexcept;

    std::size_t num_vars() const noexcept;

    /// @brief Bit count, i.e. `num_vars() + 1`; zero for a null handle.
    std::size_t num_bits() const noexcept { return storage_ ? storage_->num_bits() : 0; }

    bool is_null() const noexcept { return storage_ == nullptr; }

    /// @brief Resize to hold `num_vars` variables, zeroing any new bits.
    void resize(std::size_t num_vars);

    /// @brief Become the true equation @f$\mathbf{0}@f$, keeping the width.
    void set_eq_true() noexcept;

    /// @brief First set bit, or `std::nullopt` for the true equation.
    std::optional<std::size_t> leading_bit() const noexcept { return storage_->leading_bit(); }

    ReduceResult classify() const noexcept { return storage_->classify(); }

    bool is_eq_true() const noexcept {
        return storage_->classify().kind == ReduceResult::Kind::eq_true;
    }

    bool is_eq_false() const noexcept {
        return storage_->classify().kind == ReduceResult::Kind::eq_false;
    }

    /// @brief Add `other` into this equation over GF(2).
    /// @pre `other` shares this equation's representation and width.
    void plus_assign(const Equation& other);

    /// @brief Overwrite this equation's contents from `other`, without
    /// allocating. This is what makes the preallocated-scratch discipline
    /// possible: refill a long-lived buffer instead of returning a fresh
    /// `Equation` by value on a hot path.
    /// @pre `other` shares this equation's representation and width.
    void assign_from(const Equation& other);

    /// @brief Overwrite this equation to have exactly the given bits set.
    ///
    /// The in-place counterpart of `from_bits`, for the same reason
    /// `assign_from` is the counterpart of copy construction. It is also the
    /// coarse operation behind rewriting an equation through a fixed index
    /// permutation, as building a `TrailBasisSpace` row from reduction
    /// coefficients needs: collect the mapped bits, then assign in one dispatch.
    /// @param bits Bits to set; need not be sorted or distinct, may include the
    /// constant bit `num_vars()`, and must each be `< num_bits()`.
    void assign_bits(std::span<const std::size_t> bits);

    /// @brief Old active variable numbering to the compacted one.
    /// @pre `num_bits() == transform.old_num_vars() + 1`.
    Equation compact(const CompactionTransform& transform) const;

    /// @brief Compacted numbering back to the pre-compaction one.
    /// @pre `num_bits() == transform.new_num_vars() + 1`.
    Equation uncompact(const CompactionTransform& transform) const;

    /// @brief Collect every set bit, ascending, into `out`, cleared first. This
    /// is how code outside the algebra layer reaches set bits -- printing,
    /// DIMACS output, proof lines: one dispatch, one pass, into a reusable
    /// buffer.
    ///
    /// @note An out-parameter rather than a return value because the callers
    /// refill one buffer in a loop: `TrailBasisSpace::to_RowSpace()` once per
    /// trail-basis row, `LrupLogger` once per proof line. Returning a vector
    /// would allocate on every iteration.
    void bits(std::vector<std::size_t>& out) const;

    /// @brief Null handles compare equal to each other only.
    bool operator==(const Equation& other) const noexcept;

    std::size_t hash() const noexcept { return storage_ ? storage_->hash() : 0; }

    /// @brief Render as e.g. `[x1 + x3 = 1]`.
    std::string to_string(const PrintMode& mode = {}) const;

    /// @brief For use inside `src/algebra/` only.
    EquationInterface& storage() noexcept { return *storage_; }

    const EquationInterface& storage() const noexcept { return *storage_; }

private:
    std::unique_ptr<EquationInterface> storage_;
};

/// @brief Hash functor so `Equation` can key a `std::unordered_map`. A
/// `std::hash` specialization would require opening `namespace std`, which the
/// project's single-namespace rule forbids.
struct EquationHash {
    std::size_t operator()(const Equation& equation) const noexcept { return equation.hash(); }
};

// The two factories below add the constant bit, so widths are `num_vars + 1`.

std::unique_ptr<EquationInterface> make_equation_interface(EquationRepr repr,
                                                           std::size_t num_vars);

/// @brief Build the true equation @f$\mathbf{0}@f$ in the chosen representation.
Equation make_equation(EquationRepr repr, std::size_t num_vars);

} // namespace xorcle
