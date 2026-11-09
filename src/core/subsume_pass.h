#pragma once

/// @file subsume_pass.h
/// @brief Whether one clause subsumes or strengthens another, and the clause a
/// strengthening leaves.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "algebra/equation.h"
#include "algebra/equation_repr.h"
#include "algebra/row_echelon_matrix.h"
#include "util/index_set.h"
#include "util/random.h"

namespace xorcle {

/// @brief What a clause C is to a clause D, writing W for a clause's span.
enum class PairRelation : std::uint8_t {
    none,
    subsumes,   ///< W_C ⊆ W_D: C implies D.
    strengthens ///< W_C ⊆ W_D + ⟨0 = 1⟩ and not W_C ⊆ W_D: C and D imply a hyperplane of W_D.
};

/// @brief Reduces one clause's rows against another's, and counts the work.
class PairCheck {
public:
    PairCheck(EquationRepr repr, std::size_t num_vars);

    /// @brief How @p narrower relates to @p wider, reducing its rows against
    /// those of @p wider and stopping at the first that reduces to neither
    /// `0 = 0` nor `0 = 1`.
    /// @pre Both are over `num_vars` variables, neither spans `0 = 1`, and
    /// @p narrower has rows.
    PairRelation relate(const Clause& narrower, const Clause& wider);

    /// @brief A hyperplane of the span of @p wider that the two clauses of the
    /// last `relate()` imply together, drawn uniformly from all such.
    /// @pre The last `relate()` was on @p wider and found `strengthens`.
    Clause strengthened(const Clause& wider, Rng& rng) const;

    std::size_t num_row_checks() const noexcept { return num_row_checks_; }

    std::size_t num_row_additions() const noexcept { return num_row_additions_; }

    /// @brief Per row of a narrower clause reduced, the rows of the wider clause.
    std::size_t num_rows_read() const noexcept { return num_rows_read_; }

private:
    EquationRepr repr_;
    std::size_t num_vars_;
    Equation reduced_;

    /// @brief Per row of the last narrower clause, the rows of the wider one it
    /// reduced by, and whether it reduced to `0 = 1`.
    std::vector<IndexSet> coefficients_;
    std::vector<std::uint8_t> reduces_to_eq_false_;
    std::size_t num_narrower_rows_ = 0;

    std::size_t num_row_checks_ = 0;
    std::size_t num_row_additions_ = 0;
    std::size_t num_rows_read_ = 0;
};

} // namespace xorcle
