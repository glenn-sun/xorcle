#include "heuristics/decisions/decision_utils.h"

#include <cassert>
#include <cstdint>
#include <span>
#include <utility>

#include "algebra/reduce_result.h"
#include "util/bitmap.h"

namespace xorcle {

namespace {

#ifndef NDEBUG
/// @brief Whether some combination of a clause's rows reduces to a legal decision.
///
/// Echelonizes the clause's rows after reduction by the trail span. The result
/// has a row exactly when some combination survives reduction, and has the
/// constant column as a pivot exactly when @f$\mathbf{1}@f$ lies in the reduced
/// clause span — which is the case `sample_equation_from_clause()` rejects and
/// its precondition rules out.
bool clause_admits_a_legal_decision(const Clause& clause, const RowSpace& trail_span) {
    RowSpace reduced_clause_space(clause.backend(), clause.num_vars());
    Equation reduced_row(clause.backend(), clause.num_vars());
    for (std::size_t row_index = 0; row_index < clause.rows(); ++row_index) {
        reduced_row.assign_from(clause.row(row_index));
        trail_span.reduce(reduced_row);
        reduced_clause_space.add_row(reduced_row);
    }
    if (reduced_clause_space.rows() == 0) {
        return false;
    }
    // In row-echelon form the constant column is a pivot exactly when some row
    // *is* the false equation, so this is the "1 is in the reduced span" test.
    return !reduced_clause_space.pivot_columns().test(reduced_clause_space.cols() - 1);
}
#endif

} // namespace

ClauseSatisfactionWorkspace::ClauseSatisfactionWorkspace(Algebra backend, std::size_t num_vars)
    : backend_(backend),
      num_vars_(num_vars),
      false_equation_(Equation::eq_false(backend, num_vars)),
      scratch_false_(backend, num_vars),
      scratch_row_(backend, num_vars),
      local_basis_(backend, num_vars) {}

void ClauseSatisfactionWorkspace::reconfigure(std::size_t num_vars) {
    num_vars_ = num_vars;

    const std::size_t constant_column = num_vars;
    false_equation_.resize(num_vars);
    false_equation_.assign_columns(std::span<const std::size_t>(&constant_column, 1));

    scratch_false_.resize(num_vars);
    scratch_false_.clear();
    scratch_row_.resize(num_vars);
    scratch_row_.clear();

    local_basis_ = RowSpace(backend_, num_vars);
}

void ClauseSatisfactionWorkspace::clear_local_basis() {
    while (local_basis_.rows() != 0) {
        local_basis_.pop_last_row();
    }
}

const Equation& decide_from_basis(const std::vector<Equation>& basis, const RowSpace& trail_span,
                                  Equation& reduced_scratch) {
    assert(!basis.empty() && "decide_from_basis: empty basis");
    for (const Equation& candidate : basis) {
        assert(candidate.size() == trail_span.cols());
        reduced_scratch.assign_from(candidate);
        if (trail_span.partial_reduce(reduced_scratch).is_leading()) {
            return candidate;
        }
    }
    assert(false && "decide_from_basis: no legal decision remained");
    return basis.front();
}

bool clause_satisfied_by_trail(const RowSpace& trail_span, const Clause& clause,
                               ClauseSatisfactionWorkspace& workspace) {
    assert(workspace.num_vars_ + 1 == trail_span.cols());
    assert(clause.cols() == trail_span.cols());

    workspace.clear_local_basis();

    // The false equation is reduced once by the trail span here, then reduced
    // incrementally by each clause row as the local basis grows. Recomputing it
    // from scratch per row would be the same answer at quadratic cost.
    workspace.scratch_false_.assign_from(workspace.false_equation_);
    trail_span.reduce(workspace.scratch_false_);
    if (workspace.scratch_false_.is_eq_true()) {
        // 1 is already in the trail span, so every clause is vacuously satisfied.
        return true;
    }

    for (std::size_t row_index = 0; row_index < clause.rows(); ++row_index) {
        workspace.scratch_row_.assign_from(clause.row(row_index));
        trail_span.reduce(workspace.scratch_row_);
        if (!workspace.local_basis_.add_row_get_index(workspace.scratch_row_).has_value()) {
            // Dependent on the rows already in the local basis; it adds nothing.
            continue;
        }
        if (workspace.local_basis_.partial_reduce(workspace.scratch_false_).kind ==
            ReduceResult::Kind::eq_true) {
            return true;
        }
    }
    return false;
}

std::vector<Equation> project_basis_keep_order(const std::vector<Equation>& basis,
                                               const CompactionTransform& transform,
                                               Algebra backend) {
    std::vector<Equation> projected_basis;
    projected_basis.reserve(transform.new_num_vars());
    std::vector<std::uint8_t> seen_literal(transform.new_num_vars(), 0);

    for (const Equation& basis_equation : basis) {
        assert(basis_equation.size() == transform.old_num_vars() + 1);
        Equation projected = basis_equation.project(transform);
        const ReduceResult classified = projected.classify();
        if (!classified.is_leading()) {
            // The projection collapsed to 0 or to 1; neither is a decision.
            continue;
        }
        if (seen_literal[classified.column] != 0) {
            continue;
        }
        seen_literal[classified.column] = 1;
        projected_basis.push_back(std::move(projected));
    }

    for (std::size_t var = 0; var < transform.new_num_vars(); ++var) {
        if (seen_literal[var] == 0) {
            projected_basis.push_back(Equation::from_columns(
                backend, transform.new_num_vars(), std::span<const std::size_t>(&var, 1)));
        }
    }
    return projected_basis;
}

bool sample_equation_from_clause(const Clause& clause, const RowSpace& trail_span, Rng& rng,
                                 Equation& out_candidate, Equation& out_reduced) {
    assert(clause.rows() > 0);
    assert(clause.cols() == trail_span.cols());
    assert(out_candidate.size() == trail_span.cols());
    assert(out_reduced.size() == trail_span.cols());
    assert(clause_admits_a_legal_decision(clause, trail_span) &&
           "sample_equation_from_clause: the clause is already decided or satisfied");

    while (true) {
        const Bitmap coefficients = random_nonzero_coefficients(clause.rows(), rng);
        out_candidate.clear();
        coefficients.for_each_set(
            [&](std::size_t row_index) { out_candidate.xor_assign(clause.row(row_index)); });
        out_reduced.assign_from(out_candidate);
        if (trail_span.partial_reduce(out_reduced).is_leading()) {
            return true;
        }
    }
}

} // namespace xorcle
