#include "util/DecisionUtils.h"

#include <cassert>

namespace util {
algebra::Equation decide_from_basis(const std::vector<algebra::Equation>& basis,
                                    const algebra::RowSpace& trail_span,
                                    std::size_t num_vars) {
    algebra::Equation eqn{num_vars}, eqn_reduced{num_vars}, eqn_not_reduced{num_vars};
    for (std::size_t i = 0; i < num_vars; ++i) {
        assert(eqn.size() == basis[i].size());
        eqn.unsafe_copy(basis[i]);
        assert(eqn_reduced.size() == eqn.size());
        eqn_reduced.unsafe_copy(eqn);
        trail_span.reduce(eqn_reduced);
        assert(eqn_not_reduced.size() == eqn_reduced.size());
        eqn_not_reduced.unsafe_copy(eqn_reduced);
        eqn_not_reduced.flip(num_vars);
        if (eqn_reduced.any() && eqn_not_reduced.any()) {
            return eqn;
        }
    }
    assert(false);
    return eqn;
}

algebra::Equation decide_from_units(const algebra::RowSpace& trail_span,
                                    const std::vector<algebra::Equation>& unit_eqns,
                                    std::size_t num_vars) {
    algebra::Equation eqn{num_vars}, eqn_reduced{num_vars}, eqn_not_reduced{num_vars};
    for (std::size_t i = 0; i < num_vars; ++i) {
        assert(i < unit_eqns.size());
        assert(eqn.size() == unit_eqns[i].size());
        eqn.unsafe_copy(unit_eqns[i]);
        assert(eqn_reduced.size() == eqn.size());
        eqn_reduced.unsafe_copy(eqn);
        trail_span.reduce(eqn_reduced);
        assert(eqn_not_reduced.size() == eqn_reduced.size());
        eqn_not_reduced.unsafe_copy(eqn_reduced);
        eqn_not_reduced.flip(num_vars);
        if (eqn_reduced.any() && eqn_not_reduced.any()) {
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
    combined.reduce(eqn_false);
    return eqn_false.none();
}
} // namespace util
