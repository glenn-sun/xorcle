#include "util/DecisionUtils.h"

#include <cassert>

#include "bit/vector.h"

namespace util {
algebra::Equation decide_from_basis(const std::vector<algebra::Equation>& basis,
                                    const algebra::RowSpace& trail_span,
                                    std::size_t num_vars) {
    algebra::Equation eqn{num_vars}, eqn_reduced{num_vars};
    for (std::size_t i = 0; i < num_vars; ++i) {
        assert(eqn.size() == basis[i].size());
        eqn.unsafe_copy(basis[i]);
        assert(eqn_reduced.size() == eqn.size());
        eqn_reduced.unsafe_copy(eqn);
        algebra::ReduceResult rr = trail_span.partial_reduce(eqn_reduced);
        if (rr == algebra::ReduceResult::Nontrivial) {
            return eqn;
        }
    }
    assert(false);
    return eqn;
}

algebra::Equation decide_from_units(const algebra::RowSpace& trail_span,
                                    const std::vector<algebra::Equation>& unit_eqns,
                                    std::size_t num_vars) {
    algebra::Equation eqn{num_vars}, eqn_reduced{num_vars};
    for (std::size_t i = 0; i < num_vars; ++i) {
        assert(i < unit_eqns.size());
        assert(eqn.size() == unit_eqns[i].size());
        eqn.unsafe_copy(unit_eqns[i]);
        assert(eqn_reduced.size() == eqn.size());
        eqn_reduced.unsafe_copy(eqn);
        algebra::ReduceResult rr = trail_span.partial_reduce(eqn_reduced);
        if (rr == algebra::ReduceResult::Nontrivial) {
            return eqn;
        }
    }
    assert(false);
    return eqn;
}

bool clause_satisfied_by_trail(const algebra::RowSpace& trail_span,
                               const algebra::Clause& clause,
                               std::size_t num_vars) {
    algebra::RowSpace combined = trail_span;
    for (std::size_t i = 0; i < clause.rows(); ++i) {
        combined.add_row(clause.row(i));
    }
    algebra::Equation eqn_false = algebra::Equation::unit(num_vars + 1, num_vars);
    return combined.partial_reduce(eqn_false) == algebra::ReduceResult::Zero;
}

bool sample_equation_from_clause(const algebra::Clause& clause,
                                 const algebra::RowSpace& trail_span,
                                 std::size_t num_vars,
                                 algebra::Equation& out_candidate,
                                 algebra::Equation& out_reduced) {
    if (clause.rows() == 0) {
        return false;
    }
    if (out_candidate.size() != num_vars) {
        out_candidate = algebra::Equation{num_vars};
    }
    if (out_reduced.size() != num_vars) {
        out_reduced = algebra::Equation{num_vars};
    }
    while (true) {
        bit::vector<> coeffs = bit::vector<>::random(clause.rows());
        out_candidate.reset();
        coeffs.if_set_call([&](std::size_t i) {
            out_candidate ^= clause.row(i);
        });
        assert(out_reduced.size() == out_candidate.size());
        out_reduced.unsafe_copy(out_candidate);
        algebra::ReduceResult rr = trail_span.partial_reduce(out_reduced);
        if (rr != algebra::ReduceResult::Zero) {
            return true;
        }
    }
}
} // namespace util
