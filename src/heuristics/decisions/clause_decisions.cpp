#include "heuristics/decisions/clause_decisions.h"

#include <cassert>
#include <cstdint>
#include <span>
#include <utility>

#include "algebra/reduce_result.h"
#include "util/index_set.h"

namespace xorcle {

namespace {

#ifndef NDEBUG
/// @brief Whether some combination of a clause's rows reduces to a legal
/// decision, by echelonizing the rows after reduction by the trail span. This is
/// `sample_equation_from_clause()`'s precondition, checked directly.
bool clause_admits_a_legal_decision(const Clause& clause, const RowSpace& trail_span) {
    RowSpace reduced_clause_space(clause.repr(), clause.num_vars());
    Equation reduced_row(clause.repr(), clause.num_vars());
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

ClauseSatisfactionWorkspace::ClauseSatisfactionWorkspace(EquationRepr repr, std::size_t num_vars)
    : repr_(repr),
      num_vars_(num_vars),
      false_equation_(Equation::eq_false(repr, num_vars)),
      scratch_false_(repr, num_vars),
      scratch_row_(repr, num_vars),
      local_basis_(repr, num_vars) {}

void ClauseSatisfactionWorkspace::reconfigure(std::size_t num_vars) {
    num_vars_ = num_vars;

    const std::size_t constant_bit = num_vars;
    false_equation_.resize(num_vars);
    false_equation_.assign_bits(std::span<const std::size_t>(&constant_bit, 1));

    scratch_false_.resize(num_vars);
    scratch_false_.set_eq_true();
    scratch_row_.resize(num_vars);
    scratch_row_.set_eq_true();

    local_basis_ = RowSpace(repr_, num_vars);
}

void ClauseSatisfactionWorkspace::clear_local_basis() {
    while (local_basis_.rows() != 0) {
        local_basis_.pop_last_row();
    }
}

const Equation& decide_from_basis(const std::vector<Equation>& basis,
                                  const RowSpace& trail_span) {
    assert(!basis.empty() && "decide_from_basis: empty basis");
    // The per-candidate bit count and representation are checked one layer down,
    // by `Equation::assign_from()`.
    Equation reduced(trail_span.repr(), trail_span.num_vars());
    for (const Equation& candidate : basis) {
        reduced.assign_from(candidate);
        if (trail_span.partial_reduce(reduced).is_leading()) {
            return candidate;
        }
    }
    assert(false && "decide_from_basis: no legal decision remained");
    return basis.front();
}

bool clause_satisfied_by_trail(const RowSpace& trail_span, const Clause& clause,
                               ClauseSatisfactionWorkspace& workspace) {
    assert(workspace.num_vars_ + 1 == trail_span.cols() &&
           "the workspace was configured for a different variable count");
    assert(workspace.repr_ == trail_span.repr() &&
           "the workspace was configured for a different representation");
    assert(clause.cols() == trail_span.cols() &&
           "clause and trail span disagree on the column count");

    workspace.clear_local_basis();

    // Reduced once by the trail span here, then incrementally by each clause row
    // as the local basis grows; recomputing per row costs quadratic for nothing.
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

std::vector<Equation> compact_basis_keep_order(const std::vector<Equation>& basis,
                                               const CompactionTransform& transform,
                                               EquationRepr repr) {
    std::vector<Equation> compacted_basis;
    compacted_basis.reserve(transform.new_num_vars());
    std::vector<std::uint8_t> seen_literal(transform.new_num_vars(), 0);

    for (const Equation& basis_equation : basis) {
        assert(basis_equation.repr() == repr &&
               "compact_basis_keep_order: basis and target representation disagree");
        // The bit count is checked one layer down, by `Equation::compact()`.
        Equation compacted = basis_equation.compact(transform);
        const ReduceResult classified = compacted.classify();
        if (!classified.is_leading()) {
            // The compaction collapsed it to 0 or to 1; neither is a decision.
            continue;
        }
        if (seen_literal[classified.bit] != 0) {
            continue;
        }
        seen_literal[classified.bit] = 1;
        compacted_basis.push_back(std::move(compacted));
    }

    for (std::size_t var = 0; var < transform.new_num_vars(); ++var) {
        if (seen_literal[var] == 0) {
            compacted_basis.push_back(Equation::from_bits(
                repr, transform.new_num_vars(), std::span<const std::size_t>(&var, 1)));
        }
    }
    return compacted_basis;
}

SampledDecision sample_equation_from_clause(const Clause& clause, const RowSpace& trail_span,
                                            Rng& rng) {
    assert(clause.rows() > 0 && "sample_equation_from_clause: empty clause");
    assert(clause.cols() == trail_span.cols() &&
           "sample_equation_from_clause: clause and trail span disagree on the column count");
    assert(clause.repr() == trail_span.repr() &&
           "sample_equation_from_clause: clause and trail span use different representations");
    assert(clause_admits_a_legal_decision(clause, trail_span) &&
           "sample_equation_from_clause: the clause is already decided or satisfied");

    SampledDecision sample{Equation(trail_span.repr(), trail_span.num_vars()),
                           Equation(trail_span.repr(), trail_span.num_vars())};
    while (true) {
        const IndexSet coefficients = random_nonzero_coefficients(clause.rows(), rng);
        sample.candidate.set_eq_true();
        coefficients.for_each_set(
            [&](std::size_t row_index) { sample.candidate.plus_assign(clause.row(row_index)); });
        sample.reduced.assign_from(sample.candidate);
        if (trail_span.partial_reduce(sample.reduced).is_leading()) {
            return sample;
        }
    }
}

} // namespace xorcle
