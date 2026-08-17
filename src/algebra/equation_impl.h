#pragma once

#include <cassert>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "algebra/algebra.h"
#include "algebra/compaction_transform.h"
#include "algebra/equation.h"
#include "algebra/reduce_result.h"
#include "algebra/row.h"
#include "util/print_mode.h"

namespace xorcle {

/// @brief The one implementation of `EquationStorage`, written against `Row`.
///
/// Instantiated once per backend, in that backend's own translation unit. Every
/// per-column loop -- projection, lifting, column collection, formatting --
/// lives here, inside the virtual boundary: a single dispatch from the caller's
/// point of view and a fully inlined direct call from `R`'s.
template <Row R>
class EquationImpl final : public EquationStorage {
public:
    explicit EquationImpl(std::size_t width) : row_(width) {}

    explicit EquationImpl(R row) : row_(std::move(row)) {}

    const R& row() const noexcept { return row_; }

    R& row() noexcept { return row_; }

    Algebra backend() const noexcept override { return R::algebra; }

    std::unique_ptr<EquationStorage> clone() const override {
        return std::make_unique<EquationImpl>(*this);
    }

    std::size_t size() const noexcept override { return row_.width(); }

    void resize(std::size_t width) override { row_.resize(width); }

    void clear() noexcept override { row_.clear(); }

    void assign_columns(std::span<const std::size_t> columns) override {
        row_.assign_columns(columns);
    }

    std::optional<std::size_t> leading_column() const noexcept override {
        return row_.leading_column();
    }

    ReduceResult classify() const noexcept override { return row_.classify(); }

    void xor_assign(const EquationStorage& other) override { row_.xor_assign(cast(other).row_); }

    void assign_from(const EquationStorage& other) override { row_.assign_from(cast(other).row_); }

    std::unique_ptr<EquationStorage> project(const CompactionTransform& transform) const override {
        R out(transform.new_num_vars() + 1);
        const std::size_t old_num_vars = transform.old_num_vars();
        const std::size_t new_num_vars = transform.new_num_vars();
        row_.for_each_column([&](std::size_t old_column) {
            if (old_column == old_num_vars) {
                out.set(new_num_vars);
                return;
            }
            const std::size_t mapped = transform.to_new(old_column);
            if (mapped != CompactionTransform::npos) {
                out.set(mapped);
            }
        });
        return std::make_unique<EquationImpl>(std::move(out));
    }

    std::unique_ptr<EquationStorage> lift(const CompactionTransform& transform) const override {
        R out(transform.old_num_vars() + 1);
        const std::size_t old_num_vars = transform.old_num_vars();
        const std::size_t new_num_vars = transform.new_num_vars();
        row_.for_each_column([&](std::size_t new_column) {
            out.set(new_column == new_num_vars ? old_num_vars : transform.to_old(new_column));
        });
        return std::make_unique<EquationImpl>(std::move(out));
    }

    void columns(std::vector<std::size_t>& out) const override {
        row_.for_each_column([&](std::size_t column) { out.push_back(column); });
    }

    bool equals(const EquationStorage& other) const noexcept override {
        return row_.equals(cast(other).row_);
    }

    std::size_t hash() const noexcept override { return row_.hash(); }

    std::string to_string(const PrintMode& mode) const override {
        std::ostringstream oss;
        const std::size_t constant_column = row_.width() - 1;
        row_.for_each_column([&](std::size_t column) {
            if (column == constant_column) {
                return;
            }
            if (oss.tellp() > 0) {
                oss << " + ";
            }
            oss << "x" << (column + 1);
        });

        bool constant = row_.test(constant_column);
        if (mode.clause_mode == ClausePrintMode::as_clause) {
            constant = !constant;
        }
        switch (mode.equation_mode) {
            case EquationPrintMode::standard:
                if (oss.tellp() == 0) {
                    oss << "0";
                }
                oss << " = " << (constant ? "1" : "0");
                break;
            case EquationPrintMode::equals_0:
                if (oss.tellp() == 0) {
                    oss << (constant ? "1" : "0") << " = 0";
                } else {
                    oss << (constant ? " + 1" : "") << " = 0";
                }
                break;
            case EquationPrintMode::equals_1:
                if (oss.tellp() == 0) {
                    oss << (constant ? "0" : "1") << " = 1";
                } else {
                    oss << (constant ? "" : " + 1") << " = 1";
                }
                break;
        }
        return "[" + oss.str() + "]";
    }

private:
    static const EquationImpl& cast(const EquationStorage& storage) noexcept {
        assert(storage.backend() == R::algebra && "backend mismatch in EquationImpl");
        return static_cast<const EquationImpl&>(storage);
    }

    R row_;
};

/// @brief Reach the concrete row behind an `Equation` handle.
/// @pre `!equation.empty()` and `equation.backend() == R::algebra`; likewise for
/// the mutable overload below.
template <Row R>
const R& row_of(const Equation& equation) noexcept {
    assert(!equation.empty() && equation.backend() == R::algebra);
    return static_cast<const EquationImpl<R>&>(equation.storage()).row();
}

template <Row R>
R& row_of(Equation& equation) noexcept {
    assert(!equation.empty() && equation.backend() == R::algebra);
    return static_cast<EquationImpl<R>&>(equation.storage()).row();
}

/// @brief Wrap a concrete row in an `Equation` handle.
template <Row R>
Equation make_equation_from_row(R row) {
    return Equation(std::make_unique<EquationImpl<R>>(std::move(row)));
}

} // namespace xorcle
