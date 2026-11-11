#pragma once

/// @file equation_pool.h
/// @brief Equations kept for reuse, so that building rows allocates only when none is kept.

#include <cassert>
#include <cstddef>
#include <utility>
#include <vector>

#include "algebra/equation.h"
#include "algebra/equation_repr.h"
#include "algebra/row_echelon_matrix.h"

namespace xorcle {

/// @brief Spare equations of one representation and one bit count, which a taker overwrites.
class EquationPool {
public:
    /// @pre `num_bits > 0`.
    EquationPool(EquationRepr repr, std::size_t num_bits) noexcept
        : repr_(repr), num_bits_(num_bits) {
        assert(num_bits > 0 && "an equation has at least its constant bit");
    }

    EquationRepr repr() const noexcept { return repr_; }

    std::size_t num_bits() const noexcept { return num_bits_; }

    /// @brief An equation with any contents.
    Equation take() {
        if (kept_.empty()) {
            return Equation::with_num_bits(repr_, num_bits_);
        }
        Equation equation = std::move(kept_.back());
        kept_.pop_back();
        return equation;
    }

    /// @pre `source` has this pool's representation and bit count.
    Equation copy_of(const Equation& source) {
        assert(!source.is_null() && source.repr() == repr_ && source.num_bits() == num_bits_ &&
               "copy_of: the source has another representation or bit count");
        if (kept_.empty()) {
            return source;
        }
        Equation equation = std::move(kept_.back());
        kept_.pop_back();
        equation.assign_from(source);
        return equation;
    }

    /// @pre `equation` has this pool's representation and bit count.
    void give(Equation&& equation) {
        assert(!equation.is_null() && equation.repr() == repr_ &&
               equation.num_bits() == num_bits_ &&
               "give: the equation has another representation or bit count");
        kept_.push_back(std::move(equation));
    }

    /// @brief Empty `matrix`, keeping its rows.
    /// @pre `matrix` has this pool's representation, and as many columns as it has bits.
    void give_rows_of(RowEchelonMatrix& matrix) {
        assert(matrix.repr() == repr_ && matrix.cols() == num_bits_ &&
               "give_rows_of: the matrix has another representation or column count");
        while (matrix.rows() != 0) {
            kept_.push_back(matrix.pop_row(matrix.rows() - 1));
        }
    }

    /// @brief Keep every equation, resized to `num_bits` bits.
    /// @pre `num_bits > 0`.
    void resize(std::size_t num_bits) {
        assert(num_bits > 0 && "an equation has at least its constant bit");
        if (num_bits == num_bits_) {
            return;
        }
        num_bits_ = num_bits;
        for (Equation& equation : kept_) {
            equation.resize(num_bits - 1);
        }
    }

    /// @brief Free every equation, and take `num_bits` bits from now on.
    /// @pre `num_bits > 0`.
    void clear(std::size_t num_bits) noexcept {
        assert(num_bits > 0 && "an equation has at least its constant bit");
        num_bits_ = num_bits;
        kept_.clear();
    }

private:
    EquationRepr repr_;
    std::size_t num_bits_;
    std::vector<Equation> kept_;
};

} // namespace xorcle
