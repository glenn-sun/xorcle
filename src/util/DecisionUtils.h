#pragma once

#include <cstddef>
#include <vector>

#include "algebra/Equation.h"
#include "algebra/RowSpace.h"
#include "bit/vector.h"
#include "search/Compaction.h"
#include "util/RandomUtils.h"

namespace util {

/// @brief Reusable scratch state for clause satisfaction checks.
struct ClauseSatisfactionWorkspace {
    std::size_t num_vars;

    algebra::Equation eqn_false;
    algebra::Equation scratch_false;
    algebra::Equation scratch_row;

    bit::vector<> pivot_cols;
    std::vector<std::size_t> col_to_row;
    std::vector<algebra::Equation> basis_rows;
    std::size_t basis_size = 0;

    explicit ClauseSatisfactionWorkspace(std::size_t num_vars);

    /// @brief Reconfigure workspace buffers for a new variable count.
    void reconfigure(std::size_t next_num_vars);
};

/// @brief Choose a decision from a basis of equations that is not yet implied by the trail.
/// @param basis Candidate basis rows to scan in order.
/// @param trail_span Current trail span.
/// @param num_vars Number of variables in the active problem.
/// @pre `basis.size() == num_vars`.
/// @pre Every equation in `basis` has width `num_vars + 1`.
/// @post The returned equation has width `num_vars + 1` and is not implied by `trail_span`.
/// @throws std::logic_error If the caller violated the precondition that some legal decision still exists.
algebra::Equation decide_from_basis(const std::vector<algebra::Equation>& basis,
                                    const algebra::RowSpace& trail_span,
                                    std::size_t num_vars);

/// @brief Choose a decision from unit equations that is not yet implied by the trail.
/// @param trail_span Current trail span.
/// @param unit_eqns Unit equations in variable order.
/// @param num_vars Number of variables in the active problem.
/// @pre `unit_eqns.size() == num_vars`.
/// @pre Every equation in `unit_eqns` has width `num_vars + 1`.
/// @post The returned equation has width `num_vars + 1` and is not implied by `trail_span`.
/// @throws std::logic_error If the caller violated the precondition that some legal decision still exists.
algebra::Equation decide_from_units(const algebra::RowSpace& trail_span,
                                    const std::vector<algebra::Equation>& unit_eqns,
                                    std::size_t num_vars);

/// @brief Return true if the clause is already satisfied by the trail.
/// @param trail_span Current trail span.
/// @param clause Clause to test.
/// @param ws Reused scratch workspace sized for the active variable count.
/// @pre `ws` has been configured for the active width `trail_span.cols() - 1`.
bool clause_satisfied_by_trail(const algebra::RowSpace& trail_span,
                               const algebra::Clause& clause,
                               ClauseSatisfactionWorkspace& ws);

/// @brief Project a literal basis through a compaction transform while preserving basis order.
/// @param basis Candidate literal basis in old active-variable order.
/// @param transform Compaction transform into the new active-variable space.
/// @return A complete literal basis in the new active-variable space.
/// @pre Every equation in `basis` has width `transform.old_num_vars + 1`.
/// @post The returned basis has size `transform.new_num_vars`.
std::vector<algebra::Equation> project_basis_keep_order(const std::vector<algebra::Equation>& basis,
                                                        const search::CompactionTransform& transform);

/// @brief Sample a non-zero equation from a clause with respect to the trail.
/// @param clause Clause to sample from.
/// @param trail_span Current trail span.
/// @param num_vars Number of variables in the problem.
/// @param rng RNG stream used to choose clause-row combinations.
/// @param out_candidate Output equation sampled from the clause.
/// @param out_reduced Output equation reduced by the trail span.
/// @pre The clause span contains at least one vector whose reduction by `trail_span` is non-zero.
/// @pre `out_candidate` and `out_reduced` are either empty or already have width `num_vars + 1`.
/// @post When the function returns `true`, `out_candidate` has width `num_vars + 1` and
/// `out_reduced` is its partial reduction by `trail_span`.
bool sample_equation_from_clause(const algebra::Clause& clause,
                                 const algebra::RowSpace& trail_span,
                                 std::size_t num_vars,
                                 Rng& rng,
                                 algebra::Equation& out_candidate,
                                 algebra::Equation& out_reduced);

} // namespace util
