#include "algebra/TrailBasisSpace.h"
#include "search/Trail.h"
#include <cassert>

namespace algebra {
TrailBasisSpace TrailBasisSpace::from_RowSpace(const RowSpace& rs, const search::Trail& trail) {
    TrailBasisSpace tbs(trail);
    Coefficients coeffs;
    Equation eqn_copy{trail.span().cols() - 1};
    for (const auto& eqn : rs) {
        // Coefficients in representation order
        assert(eqn_copy.size() == eqn.size());
        eqn_copy.unsafe_copy(eqn);
        trail.span().reduce(eqn_copy, &coeffs);

        // Convert to trail basis order
        std::size_t m = coeffs.size();
        Coefficients tbs_row = Coefficients::zeros(m);
        coeffs.if_set_call([&](std::size_t i) {
            tbs_row.set(m - trail.reverse_order_map()[i] - 1);
        });
        tbs.add_row(tbs_row);
    }
    return tbs;
}

RowSpace TrailBasisSpace::to_RowSpace() const {
    RowSpace rs(trail.span().cols());
    Equation eqn(trail.span().cols() - 1);
    for (const auto& tbs_row : *this) {
        eqn.reset();
        tbs_row.if_set_call([&](std::size_t i) {
            eqn += trail.span().row(trail.order_map()[cols()-i-1]);
        });
        rs.add_row(eqn);
    }
    return rs;
}

std::size_t TrailBasisSpace::jumpable_levels() const {
    if (rows() == 1) {
        return trail.size() - 1;
    }

    std::size_t jump = 1;
    bit::vector<> new_part;
    while (jump < trail.size()) {
        std::size_t last_decision = trail[trail.size() - 1 - jump].span.rows();
        std::size_t n_current_level = trail.span().rows() - last_decision;
        new_part = row(1).sub(n_current_level);
        if (new_part.any()) {
            return jump - 1;
        }
        ++jump;
    } 
    
    return jump - 1;
}
} // namespace algebra
