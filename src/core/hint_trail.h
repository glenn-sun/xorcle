#pragma once

#include <cstddef>

#include "algebra/equation.h"
#include "algebra/equation_repr.h"
#include "algebra/row_echelon_matrix.h"

namespace xorcle {

/// @brief What citing one clause does to the checker's trail.
enum class Hint {
    conflicting, ///< Every row of the clause is on the trail.
    propagated,  ///< One row is left, and its negation is now on the trail.
    known,       ///< That negation was already there, which the checker refuses.
    not_unit,    ///< Two rows are left, which the checker refuses.
};

/// @brief The checker's trail while a line's hints are chosen: the line's own rows, then
/// the negation each cited clause propagates.
class HintTrail {
public:
    HintTrail(const EquationRepr repr, const std::size_t num_vars)
        : span_(repr, num_vars),
          false_eqn_(Equation::eq_false(repr, num_vars)),
          residual_(repr, num_vars),
          scratch_(repr, num_vars) {}

    /// @brief Begin a line, whose own rows the checker assumes.
    template <class Rows>
    void restart(const Rows& rows) {
        while (span_.rows() != 0) {
            span_.pop_last_row();
        }
        for (const Equation& row : rows) {
            span_.add_row(row);
        }
    }

    bool holds(const Equation& eqn) {
        scratch_.assign_from(eqn);
        span_.reduce(scratch_);
        return scratch_.is_eq_true();
    }

    bool is_refuted() { return holds(false_eqn_); }

    /// @brief Cite the clause whose linear negation is @p rows.
    template <class Rows>
    Hint cite(const Rows& rows) {
        bool any = false;
        for (const Equation& row : rows) {
            scratch_.assign_from(row);
            span_.reduce(scratch_);
            if (scratch_.is_eq_true()) {
                continue;
            }
            if (!any) {
                residual_.assign_from(scratch_);
                any = true;
            } else if (!(residual_ == scratch_)) {
                return Hint::not_unit;
            }
        }
        if (!any) {
            return Hint::conflicting;
        }
        residual_.negate();
        if (holds(residual_)) {
            return Hint::known;
        }
        span_.add_row(residual_);
        return Hint::propagated;
    }

private:
    RowSpace span_;
    Equation false_eqn_;
    Equation residual_;
    Equation scratch_;
};

} // namespace xorcle
