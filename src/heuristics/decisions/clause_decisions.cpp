#include "heuristics/decisions/clause_decisions.h"

#include <cassert>
#include <utility>

#include "util/index_set.h"

namespace xorcle {

ClauseSatisfactionWorkspace::ClauseSatisfactionWorkspace(EquationRepr repr, std::size_t num_vars)
    : repr_(repr),
      num_vars_(num_vars),
      local_basis_(repr, num_vars),
      spare_rows_(repr, num_vars + 1) {}

void ClauseSatisfactionWorkspace::reconfigure(std::size_t num_vars) {
    num_vars_ = num_vars;
    local_basis_ = RowSpace(repr_, num_vars);
    spare_rows_.clear(num_vars + 1);
}

ClauseStatus clause_status_by_trail(const RowSpace& trail_span, const Clause& clause,
                                    ClauseSatisfactionWorkspace& workspace,
                                    std::vector<Equation>* reduced_rows) {
    assert(workspace.num_vars_ + 1 == trail_span.cols() &&
           "the workspace was configured for a different variable count");
    assert(workspace.repr_ == trail_span.repr() &&
           "the workspace was configured for a different representation");
    assert(clause.cols() == trail_span.cols() &&
           "clause and trail span disagree on the column count");

    workspace.spare_rows_.give_rows_of(workspace.local_basis_);
    if (reduced_rows != nullptr) {
        reduced_rows->clear();
    }

    if (trail_span.pivot_col_idxs().test(trail_span.cols() - 1)) {
        // `0 = 1` is already in the trail span, so every clause is vacuously satisfied.
        return ClauseStatus::satisfied;
    }
    // Otherwise the trail span leaves `0 = 1` alone, and the local basis holds it
    // exactly when a row of its own leads on the constant column.
    const std::size_t constant_col_idx = trail_span.cols() - 1;

    for (std::size_t row_idx = 0; row_idx < clause.rows(); ++row_idx) {
        Equation row = workspace.spare_rows_.copy_of(clause.row(row_idx));
        trail_span.reduce(row);
        if (reduced_rows != nullptr) {
            reduced_rows->push_back(row);
        }
        if (!workspace.local_basis_.add_row_or_keep(row).has_value()) {
            // Dependent on the rows already in the local basis; it adds nothing.
            workspace.spare_rows_.give(std::move(row));
            continue;
        }
        if (workspace.local_basis_.pivot_col_idxs().test(constant_col_idx)) {
            return ClauseStatus::satisfied;
        }
    }
    // What the local basis spans is the clause modulo the trail, so its rank is
    // the width of the clause reduced by the trail.
    return workspace.local_basis_.rows() <= 1 ? ClauseStatus::propagating : ClauseStatus::open;
}

} // namespace xorcle
