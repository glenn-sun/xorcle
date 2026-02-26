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
                                                                         std::optional<heuristics::WatchType> zero_watch_hint,
                                                                         std::size_t& num_watch_updates) {
    algebra::Clause& clause = clause_sel.get_clause(id);

    // If clause is unit, conflict/propagate are the same
    if (clause.rows() == 1) {
        assert(working_eqn.size() == clause.row(0).size());
        working_eqn.unsafe_copy(clause.row(0));
        algebra::ReduceResult rr = trail.span().partial_reduce(working_eqn);
        if (rr == algebra::ReduceResult::One) {
            return std::nullopt;
        }
        working_eqn.flip(num_vars);
        return working_eqn;
    }

    // Overwrites scratch, distinct_from prevents sum from being 0, must already be partial reduced
    auto find_replacement_cyclic = [&](algebra::Equation& scratch, const algebra::Equation& distinct_from)
            -> std::optional<std::size_t> {
        assert(clause.rows() > 1);
        std::size_t start = clause.next_watch_search_start % clause.rows();
        for (std::size_t step = 0; step < clause.rows(); ++step) {
            std::size_t i = (start + step) % clause.rows();
            if (i == clause.watched0 || i == clause.watched1) continue;
            assert(scratch.size() == clause.row(i).size());
            scratch.unsafe_copy(clause.row(i));
            algebra::ReduceResult candidate_rr = trail.span().partial_reduce_with_extra(scratch, distinct_from);
            if (candidate_rr != algebra::ReduceResult::Zero) {
                clause.next_watch_search_start = (i + 1) % clause.rows();
                return i;
            }
        }
        return std::nullopt;
    };

    struct PropagationTask {
        std::size_t* update_ptr;
        std::size_t* complem_ptr;
        heuristics::WatchType type;
    };

    std::array<PropagationTask, 2> tasks;
    std::size_t num_tasks = 0;

    // Deduce which watches to update from hint, or reduce
    // WatchType::Sum should be converted to WatchType::One by the clause selector
    if (zero_watch_hint == heuristics::WatchType::Zero) {
        tasks[num_tasks++] = {&clause.watched0, &clause.watched1, heuristics::WatchType::Zero};
    } else if (zero_watch_hint == heuristics::WatchType::One) {
        tasks[num_tasks++] = {&clause.watched1, &clause.watched0, heuristics::WatchType::One};
    } else {
        working_eqn.unsafe_copy(clause.row(clause.watched0));
        algebra::ReduceResult rr0 = trail.span().partial_reduce(working_eqn);
        if (rr0 == algebra::ReduceResult::Zero) {
            tasks[num_tasks++] = {&clause.watched0, &clause.watched1, heuristics::WatchType::Zero};
        }
        working_eqn2.unsafe_copy(clause.row(clause.watched1));
        algebra::ReduceResult rr1 = trail.span().partial_reduce_with_extra(working_eqn2, working_eqn);
        if (rr1 == algebra::ReduceResult::Zero) {
            tasks[num_tasks++] = {&clause.watched1, &clause.watched0, heuristics::WatchType::One};
        }
    }

    for (std::size_t i = 0; i < num_tasks; ++i) {
        auto& task = tasks[i];
        working_eqn2.unsafe_copy(clause.row(*task.complem_ptr));
        algebra::ReduceResult complem_rr = trail.span().partial_reduce(working_eqn2);

        if (auto replacement = find_replacement_cyclic(working_eqn, working_eqn2);
            replacement.has_value()) {
            // Try to update the watch

            std::size_t old_watch = *task.update_ptr;
            *task.update_ptr = *replacement;
            ++num_watch_updates;
            clause_sel.update_with_new_watch(id, old_watch, task.type, trail);
            
        } else {
            // No replacement, learn complement watch if not satisfied.
            // Reuse the complement reduction already computed above.
            if (complem_rr == algebra::ReduceResult::One) {
                // Clause is satisfied by complement watch
                continue;
            }
            working_eqn2.flip(num_vars);
            return working_eqn2;
        }
    }
    return std::nullopt;

    // if (zero_watch_hint == heuristics::WatchType::Zero || zero_watch_hint == heuristics::WatchType::One) {
    //     const bool hinted_first = (*zero_watch_hint == heuristics::WatchType::Zero);
    //     algebra::Equation& hinted_ref = hinted_first ? working_eqn : working_eqn2;
    //     algebra::Equation& other_ref = hinted_first ? working_eqn2 : working_eqn;
    //     std::size_t& changing_watch = hinted_first ? clause.watched0 : clause.watched1;
    //     std::size_t old_watch = changing_watch;

    //     assert(hinted_ref.size() == clause.row(changing_watch).size());
    //     hinted_ref.unsafe_copy(clause.row(changing_watch));
    //     trail.span().reduce(hinted_ref);

    //     if (hinted_ref.none()) {
    //         if (auto replacement = find_replacement_cyclic(hinted_ref, nullptr); replacement.has_value()) {
    //             changing_watch = *replacement;
    //             update_search_start_after_watch(*replacement);
    //             ++num_watch_updates;
    //             clause_sel.update_with_new_watch(id, old_watch, hinted_first ? heuristics::WatchType::Zero
    //                                                                        : heuristics::WatchType::One,
    //                                             trail);
    //             return std::nullopt;
    //         }

    //         std::size_t other_watch_idx = hinted_first ? clause.watched1 : clause.watched0;
    //         assert(other_ref.size() == clause.row(other_watch_idx).size());
    //         other_ref.unsafe_copy(clause.row(other_watch_idx));
    //         trail.span().reduce(other_ref);
    //         bool other_was_zero = other_ref.none();
    //         other_ref.flip(num_vars);
    //         if (other_ref.none()) {
    //             return std::nullopt;
    //         } else {
    //             if (other_was_zero) {
    //                 assert(other_ref == eqn_false);
    //             }
    //             return other_ref;
    //         }
    //     }
    //     // Hint was stale/inapplicable; fall through to the full path.
    // }

    // // Otherwise, check both watched equations
    // assert(working_eqn.size() == clause.row(clause.watched0).size());
    // working_eqn.unsafe_copy(clause.row(clause.watched0));
    // trail.span().reduce(working_eqn);
    // bool first_is_zero = working_eqn.none();

    // assert(working_eqn2.size() == clause.row(clause.watched1).size());
    // working_eqn2.unsafe_copy(clause.row(clause.watched1));
    // trail.span().reduce(working_eqn2);
    // bool second_is_zero = working_eqn2.none();

    // bool sum_is_zero = (working_eqn == working_eqn2);

    // if (!first_is_zero && !second_is_zero && !sum_is_zero) {
    //     // No propagation
    //     return std::nullopt;
    // } else if (first_is_zero && second_is_zero) {
    //     // Find two new watched literals
    //     // If cannot find any, propagate eqn_false
    //     // If can only find one, propagate the non-zero watch

    //     std::size_t old_watched0 = clause.watched0;

    //     // Find first new equation to watch
    //     bool found = false;
    //     std::size_t first_replacement = 0;
    //     if (auto repl = find_replacement_cyclic(working_eqn, nullptr); repl.has_value()) {
    //         first_replacement = *repl;
    //         clause.watched0 = first_replacement;
    //         update_search_start_after_watch(first_replacement);
    //         found = true;
    //     }

    //     // If none found, return conflict
    //     if (!found) {
    //         return eqn_false;
    //     } else {
    //         ++num_watch_updates;
    //         clause_sel.update_with_new_watch(id, old_watched0, heuristics::WatchType::Zero, trail);
    //     }

    //     // Continue with second equation with same i
    //     std::size_t old_watched1 = clause.watched1;

    //     found = false;
    //     if (auto repl = find_replacement_cyclic(working_eqn2, &working_eqn); repl.has_value()) {
    //         clause.watched1 = *repl;
    //         update_search_start_after_watch(*repl);
    //         found = true;
    //     }

    //     // If none found, learn clause.row(clause.watched0) + 1
    //     if (!found) { 
    //         working_eqn.flip(num_vars);
    //         // At this point, eqn may be the zero vector if the clause is true
    //         if (working_eqn.none()) {
    //             return std::nullopt;
    //         } else {
    //             return working_eqn;
    //         }
    //     } else {
    //         // Update watchlists
    //         ++num_watch_updates;
    //         clause_sel.update_with_new_watch(id, old_watched1, heuristics::WatchType::One, trail);
    //         return std::nullopt;
    //     }
    // } else {
    //     // Find one new watched literal
    //     // If cannot find, propagate the non-zero watch

    //     algebra::Equation& non_zero_ref = first_is_zero ? working_eqn2 : working_eqn;
    //     algebra::Equation& working_ref = first_is_zero ? working_eqn : working_eqn2;
    //     std::size_t& changing_watch = first_is_zero ? clause.watched0 : clause.watched1;

    //     std::size_t old_watch = changing_watch;

    //     // Find a new equation to watch
    //     bool found = false;
    //     if (auto repl = find_replacement_cyclic(working_ref, &non_zero_ref); repl.has_value()) {
    //         changing_watch = *repl;
    //         update_search_start_after_watch(*repl);
    //         found = true;
    //     }

    //     // If none found, learn clause.row(clause.watched0) + 1
    //     if (!found) { 
    //         non_zero_ref.flip(num_vars);
    //         // At this point, eqn may be the zero vector iff the clause is true
    //         if (non_zero_ref.none()) {
    //             return std::nullopt;
    //         } else {
    //             return non_zero_ref;
    //         }
    //     } else {
    //         // Update watchlists
    //         ++num_watch_updates;
    //         clause_sel.update_with_new_watch(id, old_watch, first_is_zero ? heuristics::WatchType::Zero : heuristics::WatchType::One, trail);
    //         return std::nullopt;
    //     }
    // }
}

std::optional<std::pair<algebra::Equation, heuristics::ClauseId>> PropagationEngine::unit_propagate_once(
    heuristics::ClauseSelector& clause_sel,
    const Trail& trail,
    const algebra::Equation& eqn_false,
    std::size_t& num_attempt_props,
    std::size_t& num_watch_updates) {
    while (true) {
        auto maybe_selection = clause_sel.next_with_hint();
        if (!maybe_selection.has_value()) {
            return std::nullopt;
        } else {
            heuristics::ClauseSelection selection = maybe_selection.value();
            heuristics::ClauseId id = selection.id;
            ++num_attempt_props;
            auto result = unit_prop_one_clause(clause_sel, trail, eqn_false, id, selection.zero_watch_hint, num_watch_updates);
            if (result.has_value()) {
                return std::pair<algebra::Equation, heuristics::ClauseId>{result.value(), id};
            }
        }
    }
}

} // namespace search
