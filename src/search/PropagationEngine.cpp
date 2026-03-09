#include "search/PropagationEngine.h"

#include <array>
#include <cassert>
#include <sstream>
#include <stdexcept>

namespace search {
PropagationEngine::PropagationEngine(std::size_t num_vars)
    : num_vars(num_vars),
      working_eqn(bit::vector<>::zeros(num_vars + 1)),
      working_eqn2(bit::vector<>::zeros(num_vars + 1)) {}

void PropagationEngine::reconfigure(std::size_t next_num_vars) {
    num_vars = next_num_vars;
    working_eqn.resize(num_vars + 1);
    working_eqn2.resize(num_vars + 1);
    working_eqn.reset();
    working_eqn2.reset();
}

std::optional<algebra::Equation> PropagationEngine::unit_prop_one_clause(heuristics::ClauseSelector& clause_sel,
                                                                         const Trail& trail,
                                                                         heuristics::ClauseId id,
                                                                         std::optional<heuristics::WatchType> zero_watch_hint,
                                                                         std::size_t& num_watch_updates) {
    algebra::Clause& clause = clause_sel.get_clause(id);
    const std::size_t constant_col = num_vars;
    const auto is_zero = [](std::size_t f) { return f == bit::vector<>::npos; };
    const auto is_one = [constant_col](std::size_t f) { return f == constant_col; };

    assert(clause.cols() == num_vars + 1);
    assert(clause.rows() >= 1);

    // If clause is unit, conflict/propagate are the same
    if (clause.rows() == 1) {
        if (zero_watch_hint.has_value()) {
            std::ostringstream msg;
            msg << "PropagationEngine got non-unit hint for unit clause " << id;
            throw std::logic_error(msg.str());
        }
        assert(working_eqn.size() == clause.row(0).size());
        working_eqn.unsafe_copy(clause.row(0));
        std::size_t f = trail.span().partial_reduce(working_eqn);
        if (is_one(f)) {
            return std::nullopt;
        }
        working_eqn.flip(num_vars);
        return working_eqn;
    }

    assert(clause.watched0 < clause.rows());
    assert(clause.watched1 < clause.rows());
    assert(clause.watched0 != clause.watched1);
    assert(clause.next_watch_search_start < clause.rows());

    // Overwrites scratch. `distinct_from` is already reduced and keeps replacements
    // distinct from the current complement row when the selector tracks the watch sum.
    auto find_replacement_cyclic = [&](algebra::Equation& scratch, const algebra::Equation& distinct_from)
            -> std::optional<std::size_t> {
        assert(clause.rows() > 1);
        std::size_t start = clause.next_watch_search_start % clause.rows();
        for (std::size_t step = 0; step < clause.rows(); ++step) {
            std::size_t i = (start + step) % clause.rows();
            if (i == clause.watched0 || i == clause.watched1) continue;
            assert(scratch.size() == clause.row(i).size());
            scratch.unsafe_copy(clause.row(i));
            std::size_t candidate_f = trail.span().partial_reduce_with_extra(scratch, distinct_from);
            if (!is_zero(candidate_f)) {
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

    // Deduce which watches to update from hint, or reduce.
    // Sum hints are normalized here to updating watched1.
    if (zero_watch_hint.has_value()) {
        working_eqn.unsafe_copy(clause.row(clause.watched0));
        working_eqn2.unsafe_copy(clause.row(clause.watched1));
        trail.span().reduce(working_eqn);
        trail.span().reduce(working_eqn2);
        const bool watch0_is_zero = working_eqn.none();
        const bool watch1_is_zero = working_eqn2.none();
        const bool sum_is_zero = (working_eqn == working_eqn2);
        const bool watch0_is_one = (working_eqn.first_set() == constant_col);
        const bool watch1_is_one = (working_eqn2.first_set() == constant_col);

        const auto throw_bad_hint = [&](const char* why) -> void {
            std::ostringstream msg;
            msg << "invalid zero-watch hint " << static_cast<int>(*zero_watch_hint)
                << " on clause " << id << ": " << why
                << " [watch0_zero=" << watch0_is_zero
                << ", watch1_zero=" << watch1_is_zero
                << ", sum_zero=" << sum_is_zero
                << ", watch0_one=" << watch0_is_one
                << ", watch1_one=" << watch1_is_one << "]";
            throw std::logic_error(msg.str());
        };

        switch (*zero_watch_hint) {
            case heuristics::WatchType::Zero:
                if (!watch0_is_zero) {
                    throw_bad_hint("hinted Zero trigger is not 0=0");
                }
                if (watch1_is_one) {
                    throw_bad_hint("hinted Zero trigger has companion 0=1");
                }
                tasks[num_tasks++] = {&clause.watched0, &clause.watched1, heuristics::WatchType::Zero};
                break;
            case heuristics::WatchType::One:
                if (!watch1_is_zero) {
                    throw_bad_hint("hinted One trigger is not 0=0");
                }
                if (watch0_is_one) {
                    throw_bad_hint("hinted One trigger has companion 0=1");
                }
                tasks[num_tasks++] = {&clause.watched1, &clause.watched0, heuristics::WatchType::One};
                break;
            case heuristics::WatchType::Sum:
                if (!sum_is_zero) {
                    throw_bad_hint("hinted Sum trigger is not 0=0");
                }
                if (watch1_is_one) {
                    throw_bad_hint("hinted Sum trigger has companion 0=1");
                }
                // Sum hint is normalized to updating watched1.
                tasks[num_tasks++] = {&clause.watched1, &clause.watched0, heuristics::WatchType::One};
                break;
        }
    } else {
        working_eqn.unsafe_copy(clause.row(clause.watched0));
        std::size_t f0 = trail.span().partial_reduce(working_eqn);
        if (is_zero(f0)) {
            tasks[num_tasks++] = {&clause.watched0, &clause.watched1, heuristics::WatchType::Zero};
        }
        working_eqn2.unsafe_copy(clause.row(clause.watched1));
        std::size_t f1 = trail.span().partial_reduce_with_extra(working_eqn2, working_eqn);
        if (is_zero(f1)) {
            tasks[num_tasks++] = {&clause.watched1, &clause.watched0, heuristics::WatchType::One};
        }
    }

    for (std::size_t i = 0; i < num_tasks; ++i) {
        auto& task = tasks[i];
        working_eqn2.unsafe_copy(clause.row(*task.complem_ptr));
        std::size_t complem_f = trail.span().partial_reduce(working_eqn2);

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
            if (is_one(complem_f)) {
                // Clause is satisfied by complement watch
                continue;
            }
            working_eqn2.flip(num_vars);
            return working_eqn2;
        }
    }
    return std::nullopt;
}

std::optional<std::pair<algebra::Equation, heuristics::ClauseId>> PropagationEngine::unit_propagate_once(
    heuristics::ClauseSelector& clause_sel,
    const Trail& trail,
    std::size_t& num_attempt_props,
    std::size_t& num_watch_updates) {
    while (true) {
        auto maybe_selection = clause_sel.next_with_hint();
        if (!maybe_selection.has_value()) {
            return std::nullopt;
        } else {
            heuristics::ClauseSelection selection = maybe_selection.value();
            heuristics::ClauseId id = selection.id;
            if (id >= clause_sel.get_num_clauses()) {
                std::ostringstream msg;
                msg << "selector returned clause id " << id
                    << " with only " << clause_sel.get_num_clauses() << " clauses";
                throw std::logic_error(msg.str());
            }
            if (!clause_sel.is_clause_active(id)) {
                std::ostringstream msg;
                msg << "selector returned inactive clause id " << id;
                throw std::logic_error(msg.str());
            }
            ++num_attempt_props;
            auto result = unit_prop_one_clause(clause_sel, trail, id, selection.zero_watch_hint, num_watch_updates);
            if (result.has_value()) {
                return std::pair<algebra::Equation, heuristics::ClauseId>{result.value(), id};
            }
        }
    }
}

} // namespace search
