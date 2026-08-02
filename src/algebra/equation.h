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
/// from this through the templated `EquationImpl<Row>`; nothing outside
/// `src/algebra/` should name this type.
class EquationStorage {
public:
    virtual ~EquationStorage() = default;

    /// @brief Which backend implements this storage.
    virtual Algebra backend() const noexcept = 0;

    /// @brief Deep-copy this storage.
    virtual std::unique_ptr<EquationStorage> clone() const = 0;

    /// @brief Row width, i.e. `num_vars + 1`.
    virtual std::size_t size() const noexcept = 0;

    /// @brief Resize to `width` columns, zeroing any new ones.
    virtual void resize(std::size_t width) = 0;

    /// @brief Become the all-zero row, keeping the width.
    virtual void clear() noexcept = 0;

    /// @brief Clear, then set each listed column.
    virtual void assign_columns(std::span<const std::size_t> columns) = 0;

    /// @brief First set column, or `std::nullopt` if none is set.
    virtual std::optional<std::size_t> leading_column() const noexcept = 0;

    /// @brief Classify as the true equation, the false equation, or a leading column.
    virtual ReduceResult classify() const noexcept = 0;

    /// @brief Add another row into this one over GF(2).
    virtual void xor_assign(const EquationStorage& other) = 0;

    /// @brief Overwrite this row's contents from another of equal width.
    virtual void assign_from(const EquationStorage& other) = 0;

    /// @brief Project into a compacted variable space.
    virtual std::unique_ptr<EquationStorage> project(const CompactionTransform& transform)
        const = 0;

    /// @brief Lift back out of a compacted variable space.
    virtual std::unique_ptr<EquationStorage> lift(const CompactionTransform& transform) const = 0;

    /// @brief Append every set column, ascending, to `out` after clearing it.
    virtual void columns(std::vector<std::size_t>& out) const = 0;

    /// @brief Whether two rows of the same backend are equal.
    virtual bool equals(const EquationStorage& other) const noexcept = 0;

    /// @brief Hash of the row contents.
    virtual std::size_t hash() const noexcept = 0;

    /// @brief Render as a human-readable equation.
    virtual std::string to_string(const PrintMode& mode) const = 0;
};

/// @brief An affine equation over GF(2).
///
/// The storage width is always `num_vars + 1`: indices `[0, num_vars)` are
/// variable coefficients and index `num_vars` is the constant column. With
/// three variables the equation `x1 + x3 = 1` is `[1, 0, 1, 1]`, the false
/// equation @f$\mathbf{1} = [0 = 1]@f$ is `[0, 0, 0, 1]`, and the true equation
/// @f$\mathbf{0} = [0 = 0]@f$ is `[0, 0, 0, 0]`.
///
/// This is a handle over an `EquationStorage` chosen at construction. It is
/// deliberately missing `element`, `test`, `set`, `flip`, `blocks`, `col`, and
/// every bit iterator: exposing them would put a virtual call on each bit, which
/// costs 3x-1200x. Algorithms that need per-bit
/// access belong inside a backend, written against the `Row` concept.
class Equation {
public:
    /// @brief Construct a null handle: no storage, width 0.
    Equation() noexcept = default;

    /// @brief Construct the true equation @f$\mathbf{0}@f$ over `num_vars` variables.
    /// @param backend Storage representation to use.
    /// @param num_vars Number of variable columns; the constant column is added.
    Equation(Algebra backend, std::size_t num_vars);

    /// @brief Adopt an already-built storage.
    /// @param storage Storage to take ownership of; may be null.
    explicit Equation(std::unique_ptr<EquationStorage> storage) noexcept
        : storage_(std::move(storage)) {}

    Equation(const Equation& other);
    Equation(Equation&& other) noexcept = default;
    Equation& operator=(const Equation& other);
    Equation& operator=(Equation&& other) noexcept = default;
    ~Equation() = default;

    /// @brief The true equation @f$\mathbf{0} = [0 = 0]@f$, i.e. `[0...0 0]`.
    static Equation eq_true(Algebra backend, std::size_t num_vars);

    /// @brief The false equation @f$\mathbf{1} = [0 = 1]@f$, i.e. `[0...0 1]`.
    static Equation eq_false(Algebra backend, std::size_t num_vars);

    /// @brief Build an equation with exactly the given columns set.
    /// @param backend Storage representation to use.
    /// @param num_vars Number of variable columns.
    /// @param columns Columns to set; may include the constant column `num_vars`.
    static Equation from_columns(Algebra backend, std::size_t num_vars,
                                 std::span<const std::size_t> columns);

    /// @brief Which backend stores this equation.
    /// @pre `!empty()`.
    Algebra backend() const noexcept;

    /// @brief Number of variable columns, i.e. `size() - 1`.
    /// @pre `!empty()`.
    std::size_t num_vars() const noexcept;

    /// @brief Row width, i.e. `num_vars() + 1`; zero for a null handle.
    std::size_t size() const noexcept { return storage_ ? storage_->size() : 0; }

    /// @brief Whether this is a null handle carrying no storage.
    bool empty() const noexcept { return storage_ == nullptr; }

    /// @brief Resize to hold `num_vars` variables, zeroing any new columns.
    /// @pre `!empty()`.
    void resize(std::size_t num_vars);

    /// @brief Become the true equation @f$\mathbf{0}@f$, keeping the width.
    void clear() noexcept;

    /// @brief First set column, or `std::nullopt` for the true equation.
    /// @pre `!empty()`.
    std::optional<std::size_t> leading_column() const noexcept { return storage_->leading_column(); }

    /// @brief Classify as @f$\mathbf{0}@f$, @f$\mathbf{1}@f$, or a leading column.
    /// @pre `!empty()`.
    ReduceResult classify() const noexcept { return storage_->classify(); }

    /// @brief Whether this is the true equation @f$\mathbf{0} = [0 = 0]@f$.
    /// @pre `!empty()`.
    bool is_eq_true() const noexcept {
        return storage_->classify().kind == ReduceResult::Kind::eq_true;
    }

    /// @brief Whether this is the false equation @f$\mathbf{1} = [0 = 1]@f$.
    /// @pre `!empty()`.
    bool is_eq_false() const noexcept {
        return storage_->classify().kind == ReduceResult::Kind::eq_false;
    }

    /// @brief Add `other` into this equation over GF(2).
    /// @param other Equation to add; must share this equation's backend and width.
    void xor_assign(const Equation& other);

    /// @brief Overwrite this equation's contents from `other`, without allocating.
    ///
    /// This is the operation that makes the preallocated-scratch discipline
    /// possible: refill a long-lived buffer instead of returning a fresh
    /// `Equation` by value on a hot path.
    /// @param other Equation to copy from; must share this equation's backend and width.
    void assign_from(const Equation& other);

    /// @brief Overwrite this equation to have exactly the given columns set.
    ///
    /// The in-place counterpart of `from_columns`, for the same reason
    /// `assign_from` is the counterpart of copy construction: it refills a
    /// preallocated buffer rather than returning a fresh `Equation`.
    ///
    /// This is also the coarse operation behind rewriting an equation through a
    /// fixed index permutation, which is what building a `TrailBasisSpace` row
    /// from reduction coefficients needs: collect the mapped columns once, then
    /// assign them in a single dispatch.
    /// @param columns Columns to set; need not be sorted or distinct, and may
    /// include the constant column `num_vars()`.
    /// @pre `!empty()` and every column is `< size()`.
    void assign_columns(std::span<const std::size_t> columns);

    /// @brief Project from the old active space into the compacted space.
    /// @param transform The compaction step to apply.
    /// @return The projected equation, over `transform.new_num_vars()` variables.
    /// @pre `size() == transform.old_num_vars() + 1`.
    Equation project(const CompactionTransform& transform) const;

    /// @brief Lift from the compacted space back to the pre-compaction space.
    /// @param transform The compaction step to undo.
    /// @return The lifted equation, over `transform.old_num_vars()` variables.
    /// @pre `size() == transform.new_num_vars() + 1`.
    Equation lift(const CompactionTransform& transform) const;

    /// @brief Collect every set column, ascending, into a caller-owned buffer.
    ///
    /// This is how code outside the algebra layer reaches set bits -- printing,
    /// DIMACS output, proof lines. One dispatch, one pass, into a buffer the
    /// caller can reuse.
    /// @param out Receives the set columns; cleared first.
    void columns(std::vector<std::size_t>& out) const;

    /// @brief Compare two equations. Null handles compare equal to each other only.
    bool operator==(const Equation& other) const noexcept;

    /// @brief Hash of the equation contents; agrees with `operator==`.
    std::size_t hash() const noexcept { return storage_ ? storage_->hash() : 0; }

    /// @brief Render as a human-readable equation, e.g. `[x1 + x3 = 1]`.
    /// @param mode Rendering mode for equation and clause formatting.
    std::string to_string(const PrintMode& mode = {}) const;

    /// @brief Access the backend storage. For use inside `src/algebra/` only.
    /// @pre `!empty()`.
    EquationStorage& storage() noexcept { return *storage_; }

    /// @brief Access the backend storage. For use inside `src/algebra/` only.
    /// @pre `!empty()`.
    const EquationStorage& storage() const noexcept { return *storage_; }

private:
    std::unique_ptr<EquationStorage> storage_;
};

/// @brief Hash functor so `Equation` can key a `std::unordered_map`.
///
/// A `std::hash` specialization would require opening `namespace std`, which the
/// project's single-namespace rule forbids.
struct EquationHash {
    /// @brief Forward to `Equation::hash()`.
    std::size_t operator()(const Equation& equation) const noexcept { return equation.hash(); }
};

} // namespace xorcle
