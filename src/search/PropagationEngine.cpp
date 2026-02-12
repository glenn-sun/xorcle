#include "search/PropagationEngine.h"

#include <cassert>

namespace search {
PropagationEngine::PropagationEngine(std::size_t num_vars)
    : num_vars(num_vars),
      working_eqn(bit::vector<>::zeros(num_vars + 1)),
      working_eqn2(bit::vector<>::zeros(num_vars + 1)) {}

std::optional<algebra::Equation> PropagationEngine::unit_prop_one_clause(heuristics::ClauseSelector& clause_sel,
                                                                         const Trail& trail,
                                                                         const algebra::Equation& eqn_false,
                                                                         heuristics::ClauseId id,
                                                                         std::size_t& num_watch_updates) {
    algebra::Clause& clause = clause_sel.get_clause(id);

    // If clause is unit, conflict/propagate are the same
    if (clause.rows() == 1) {
        assert(working_eqn.size() == clause.row(0).size());
        working_eqn.unsafe_copy(clause.row(0));
        trail.span().reduce(working_eqn);
        working_eqn.flip(num_vars);
        if (working_eqn.none()) {
            return std::nullopt;
        } else {
            return working_eqn;
        }
    }

    // Otherwise, check both watched equations
    assert(working_eqn.size() == clause.row(clause.watched0).size());
    working_eqn.unsafe_copy(clause.row(clause.watched0));
    trail.span().reduce(working_eqn);
    bool first_is_zero = working_eqn.none();

    assert(working_eqn2.size() == clause.row(clause.watched1).size());
    working_eqn2.unsafe_copy(clause.row(clause.watched1));
    trail.span().reduce(working_eqn2);
    bool second_is_zero = working_eqn2.none();

    bool sum_is_zero = (working_eqn == working_eqn2);

    if (!first_is_zero && !second_is_zero && !sum_is_zero) {
        // No propagation
        return std::nullopt;
    } else if (first_is_zero && second_is_zero) {
        // Find two new watched literals
        // If cannot find any, propagate eqn_false
        // If can only find one, propagate the non-zero watch

        std::size_t old_watched0 = clause.watched0;

        // Find first new equation to watch
        std::size_t i = 0;
        bool found = false;
        for (; i < clause.rows(); ++i) {
            if (i == clause.watched0 || i == clause.watched1) continue;
            assert(working_eqn.size() == clause.row(i).size());
            working_eqn.unsafe_copy(clause.row(i));
            trail.span().reduce(working_eqn);
            if (!working_eqn.none()) {
                clause.watched0 = i;
                found = true;
                break;
            }
        }

        // If none found, return conflict
        if (!found) {
            return eqn_false;
        } else {
            ++num_watch_updates;
            clause_sel.update_with_new_watch(id, old_watched0, heuristics::WatchType::Zero, trail);
        }

        // Continue with second equation with same i
        std::size_t old_watched1 = clause.watched1;

        found = false;
        ++i;
        for (; i < clause.rows(); ++i) {
            if (i == clause.watched1) continue;
            assert(working_eqn2.size() == clause.row(i).size());
            working_eqn2.unsafe_copy(clause.row(i));
            trail.span().reduce(working_eqn2);
            if (!working_eqn2.none() && (working_eqn2 != working_eqn)) {
                clause.watched1 = i;
                found = true;
                break;
            }
        }

        // If none found, learn clause.row(clause.watched0) + 1
        if (!found) { 
            working_eqn.flip(num_vars);
            // At this point, eqn may be the zero vector if the clause is true
            if (working_eqn.none()) {
                return std::nullopt;
            } else {
                return working_eqn;
            }
        } else {
            // Update watchlists
            ++num_watch_updates;
            clause_sel.update_with_new_watch(id, old_watched1, heuristics::WatchType::One, trail);
            return std::nullopt;
        }
    } else {
        // Find one new watched literal
        // If cannot find, propagate the non-zero watch

        algebra::Equation& non_zero_ref = first_is_zero ? working_eqn2 : working_eqn;
        algebra::Equation& working_ref = first_is_zero ? working_eqn : working_eqn2;
        std::size_t& changing_watch = first_is_zero ? clause.watched0 : clause.watched1;

        std::size_t old_watch = changing_watch;

        // Find a new equation to watch
        bool found = false;
        for (std::size_t i = 0; i < clause.rows(); ++i) {
            if (i == clause.watched0 || i == clause.watched1) continue;
            assert(working_ref.size() == clause.row(i).size());
            working_ref.unsafe_copy(clause.row(i));
            trail.span().reduce(working_ref);
            if (!working_ref.none() && (working_ref != non_zero_ref)) {
                changing_watch = i;
                found = true;
                break;
            }
        }

        // If none found, learn clause.row(clause.watched0) + 1
        if (!found) { 
            non_zero_ref.flip(num_vars);
            // At this point, eqn may be the zero vector iff the clause is true
            if (non_zero_ref.none()) {
                return std::nullopt;
            } else {
                return non_zero_ref;
            }
        } else {
            // Update watchlists
            ++num_watch_updates;
            clause_sel.update_with_new_watch(id, old_watch, first_is_zero ? heuristics::WatchType::Zero : heuristics::WatchType::One, trail);
            return std::nullopt;
        }
    }
}

std::optional<std::pair<algebra::Equation, heuristics::ClauseId>> PropagationEngine::unit_propagate_once(
    heuristics::ClauseSelector& clause_sel,
    const Trail& trail,
    const algebra::Equation& eqn_false,
    std::size_t& num_attempt_props,
    std::size_t& num_watch_updates) {
    while (true) {
        auto maybe_clause = clause_sel.next();
        if (!maybe_clause.has_value()) {
            return std::nullopt;
        } else {
            heuristics::ClauseId id = maybe_clause.value();
            ++num_attempt_props;
            auto result = unit_prop_one_clause(clause_sel, trail, eqn_false, id, num_watch_updates);
            if (result.has_value()) {
                return std::pair<algebra::Equation, heuristics::ClauseId>{result.value(), id};
            }
        }
    }
}
} // namespace search
