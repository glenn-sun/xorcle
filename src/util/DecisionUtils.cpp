#include "util/DecisionUtils.h"

#include <cassert>

#include "bit/vector.h"

namespace util {
ClauseSatisfactionWorkspace::ClauseSatisfactionWorkspace(std::size_t num_vars)
    : num_vars(num_vars),
      eqn_false(algebra::Equation::unit(num_vars + 1, num_vars)),
      scratch_false(num_vars),
      scratch_row(num_vars),
      pivot_cols(bit::vector<>::zeros(num_vars + 1)),
      col_to_row(num_vars + 1, 0),
      basis_rows(),
      basis_size(0) {}

algebra::Equation decide_from_basis(const std::vector<algebra::Equation>& basis,
                                    const algebra::RowSpace& trail_span,
                                    std::size_t num_vars) {
    const std::size_t constant_col = num_vars;
    algebra::Equation eqn{num_vars}, eqn_reduced{num_vars};
    for (std::size_t i = 0; i < num_vars; ++i) {
        assert(eqn.size() == basis[i].size());
        eqn.unsafe_copy(basis[i]);
        assert(eqn_reduced.size() == eqn.size());
        eqn_reduced.unsafe_copy(eqn);
        std::size_t f = trail_span.partial_reduce(eqn_reduced);
        if ((f != bit::vector<>::npos) && (f != constant_col)) {
            return eqn;
        }
    }
    assert(false);
    return eqn;
}

algebra::Equation decide_from_units(const algebra::RowSpace& trail_span,
                                    const std::vector<algebra::Equation>& unit_eqns,
                                    std::size_t num_vars) {
    const std::size_t constant_col = num_vars;
    algebra::Equation eqn{num_vars}, eqn_reduced{num_vars};
    for (std::size_t i = 0; i < num_vars; ++i) {
        assert(i < unit_eqns.size());
        assert(eqn.size() == unit_eqns[i].size());
        eqn.unsafe_copy(unit_eqns[i]);
        assert(eqn_reduced.size() == eqn.size());
        eqn_reduced.unsafe_copy(eqn);
        std::size_t f = trail_span.partial_reduce(eqn_reduced);
        if ((f != bit::vector<>::npos) && (f != constant_col)) {
            return eqn;
        }
    }
    assert(false);
    return eqn;
}

namespace {
std::size_t partial_reduce_by_local_basis(algebra::Equation& eqn, const ClauseSatisfactionWorkspace& ws) {
    std::size_t f = eqn.first_set();
    while ((f != bit::vector<>::npos) && ws.pivot_cols.element(f)) {
        eqn ^= ws.basis_rows[ws.col_to_row[f]];
        f = eqn.next_set(f);
    }
    return f;
}
} // namespace

bool clause_satisfied_by_trail(const algebra::RowSpace& trail_span,
                               const algebra::Clause& clause,
                               ClauseSatisfactionWorkspace& ws) {
    const std::size_t width = ws.eqn_false.size();
    assert(width == ws.num_vars + 1);
    assert(ws.scratch_false.size() == width);
    assert(ws.scratch_row.size() == width);
    assert(ws.pivot_cols.size() == width);
    assert(ws.col_to_row.size() == width);
    assert(trail_span.get_pivot_cols().size() == width);
    (void)width;

    ws.basis_size = 0;
    ws.pivot_cols.reset();
    if (ws.basis_rows.size() < clause.rows()) {
        ws.basis_rows.reserve(clause.rows());
        while (ws.basis_rows.size() < clause.rows()) {
            ws.basis_rows.emplace_back(algebra::Equation(ws.num_vars));
        }
    }

    ws.scratch_false.unsafe_copy(ws.eqn_false);
    trail_span.reduce(ws.scratch_false);
    if (ws.scratch_false.none()) {
        return true;
    }

    for (std::size_t i = 0; i < clause.rows(); ++i) {
        assert(clause.row(i).size() == width);
        assert(ws.scratch_row.size() == clause.row(i).size());
        ws.scratch_row.unsafe_copy(clause.row(i));
        trail_span.reduce(ws.scratch_row);

        std::size_t row_f = partial_reduce_by_local_basis(ws.scratch_row, ws);
        if (row_f == bit::vector<>::npos) {
            continue;
        }

        assert(ws.basis_size < ws.basis_rows.size());
        ws.basis_rows[ws.basis_size].unsafe_copy(ws.scratch_row);
        ws.col_to_row[row_f] = ws.basis_size;
        ws.pivot_cols.set(row_f);
        ++ws.basis_size;

        if (partial_reduce_by_local_basis(ws.scratch_false, ws) == bit::vector<>::npos) {
            return true;
        }
    }
    return false;
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
        std::size_t f = trail_span.partial_reduce(out_reduced);
        if (f != bit::vector<>::npos) {
            return true;
        }
    }
}
} // namespace util
