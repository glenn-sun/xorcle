#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "algebra/algebra.h"
#include "algebra/compaction_transform.h"
#include "algebra/reduce_result.h"
#include "util/print_mode.h"

namespace xorcle {

class Equation;

/// @brief The abstract backend interface behind `Equation`.
///
/// Every method here is a whole-row or whole-loop operation, so exactly one
/// virtual call is paid per coarse operation and none per bit. Backends derive
/// from this through `EquationImpl<Row>`; nothing outside `src/algebra/` should
/// name this type. The operations are documented on `Equation`, which forwards.
class EquationStorage {
public:
    virtual ~EquationStorage() = default;

    virtual Algebra backend() const noexcept = 0;

    virtual std::unique_ptr<EquationStorage> clone() const = 0;

    virtual std::size_t size() const noexcept = 0;

    virtual void resize(std::size_t width) = 0;

    virtual void clear() noexcept = 0;

    virtual void assign_columns(std::span<const std::size_t> columns) = 0;

    virtual std::optional<std::size_t> leading_column() const noexcept = 0;

    virtual ReduceResult classify() const noexcept = 0;

    virtual void xor_assign(const EquationStorage& other) = 0;

    virtual void assign_from(const EquationStorage& other) = 0;

    virtual std::unique_ptr<EquationStorage> project(const CompactionTransform& transform)
        const = 0;

    virtual std::unique_ptr<EquationStorage> lift(const CompactionTransform& transform) const = 0;

    virtual void columns(std::vector<std::size_t>& out) const = 0;

    virtual bool equals(const EquationStorage& other) const noexcept = 0;

    virtual std::size_t hash() const noexcept = 0;

    virtual std::string to_string(const PrintMode& mode) const = 0;
};

/// @brief An affine equation over GF(2).
///
/// The storage width is always `num_vars + 1`: indices `[0, num_vars)` are
/// variable coefficients and index `num_vars` is the constant column. Over three
/// variables `x1 + x3 = 1` is `[1, 0, 1, 1]`, the false equation
/// @f$\mathbf{1} = [0 = 1]@f$ is `[0, 0, 0, 1]`, and the true equation
/// @f$\mathbf{0} = [0 = 0]@f$ is `[0, 0, 0, 0]`.
///
/// This is a handle over an `EquationStorage` chosen at construction. It is
/// deliberately missing `element`, `test`, `set`, `flip`, `blocks`, `col`, and
/// every bit iterator: exposing them would put a virtual call on each bit, which
/// costs orders of magnitude, whereas one virtual call per whole-row operation
/// costs nothing. Algorithms needing per-bit access belong inside a backend,
/// written against the `Row` concept.
///
/// Every method other than `size()`, `empty()`, `clear()`, `columns()`,
/// `hash()`, `to_string()` and `operator==` requires `!empty()`.
class Equation {
public:
    /// @brief A null handle: no storage, width 0.
    Equation() noexcept = default;

    /// @brief The true equation @f$\mathbf{0}@f$ over `num_vars` variables.
    Equation(Algebra backend, std::size_t num_vars);

    /// @brief Adopt an already-built storage, which may be null.
    explicit Equation(std::unique_ptr<EquationStorage> storage) noexcept
        : storage_(std::move(storage)) {}

    Equation(const Equation& other);
    Equation(Equation&& other) noexcept = default;
    Equation& operator=(const Equation& other);
    Equation& operator=(Equation&& other) noexcept = default;
    ~Equation() = default;

    static Equation eq_true(Algebra backend, std::size_t num_vars);

    static Equation eq_false(Algebra backend, std::size_t num_vars);

    /// @brief Set exactly `columns`, which may include the constant column.
    static Equation from_columns(Algebra backend, std::size_t num_vars,
                                 std::span<const std::size_t> columns);

    Algebra backend() const noexcept;

    std::size_t num_vars() const noexcept;

    /// @brief Row width, i.e. `num_vars() + 1`; zero for a null handle.
    std::size_t size() const noexcept { return storage_ ? storage_->size() : 0; }

    bool empty() const noexcept { return storage_ == nullptr; }

    /// @brief Resize to hold `num_vars` variables, zeroing any new columns.
    void resize(std::size_t num_vars);

    /// @brief Become the true equation @f$\mathbf{0}@f$, keeping the width.
    void clear() noexcept;

    /// @brief First set column, or `std::nullopt` for the true equation.
    std::optional<std::size_t> leading_column() const noexcept { return storage_->leading_column(); }

    ReduceResult classify() const noexcept { return storage_->classify(); }

    bool is_eq_true() const noexcept {
        return storage_->classify().kind == ReduceResult::Kind::eq_true;
    }

    bool is_eq_false() const noexcept {
        return storage_->classify().kind == ReduceResult::Kind::eq_false;
    }

    /// @pre `other` shares this equation's backend and width.
    void xor_assign(const Equation& other);

    /// @brief Overwrite this equation's contents from `other`, without
    /// allocating. This is what makes the preallocated-scratch discipline
    /// possible: refill a long-lived buffer instead of returning a fresh
    /// `Equation` by value on a hot path.
    /// @pre `other` shares this equation's backend and width.
    void assign_from(const Equation& other);

    /// @brief Overwrite this equation to have exactly the given columns set.
    ///
    /// The in-place counterpart of `from_columns`, for the same reason
    /// `assign_from` is the counterpart of copy construction. It is also the
    /// coarse operation behind rewriting an equation through a fixed index
    /// permutation, as building a `TrailBasisSpace` row from reduction
    /// coefficients needs: collect the mapped columns, then assign in one
    /// dispatch.
    /// @param columns Columns to set; need not be sorted or distinct, may
    /// include the constant column `num_vars()`, and must each be `< size()`.
    void assign_columns(std::span<const std::size_t> columns);

    /// @brief Old active space to compacted space.
    /// @pre `size() == transform.old_num_vars() + 1`.
    Equation project(const CompactionTransform& transform) const;

    /// @brief Compacted space back to the pre-compaction space.
    /// @pre `size() == transform.new_num_vars() + 1`.
    Equation lift(const CompactionTransform& transform) const;

    /// @brief Collect every set column, ascending, into `out`, cleared first.
    /// This is how code outside the algebra layer reaches set bits -- printing,
    /// DIMACS output, proof lines: one dispatch, one pass, into a reusable
    /// buffer.
    void columns(std::vector<std::size_t>& out) const;

    /// @brief Null handles compare equal to each other only.
    bool operator==(const Equation& other) const noexcept;

    std::size_t hash() const noexcept { return storage_ ? storage_->hash() : 0; }

    /// @brief Render as e.g. `[x1 + x3 = 1]`.
    std::string to_string(const PrintMode& mode = {}) const;

    /// @brief For use inside `src/algebra/` only.
    EquationStorage& storage() noexcept { return *storage_; }

    const EquationStorage& storage() const noexcept { return *storage_; }

private:
    std::unique_ptr<EquationStorage> storage_;
};

/// @brief Hash functor so `Equation` can key a `std::unordered_map`. A
/// `std::hash` specialization would require opening `namespace std`, which the
/// project's single-namespace rule forbids.
struct EquationHash {
    std::size_t operator()(const Equation& equation) const noexcept { return equation.hash(); }
};

} // namespace xorcle
