#include "core/propagation.h"

#include <array>
#include <cassert>
#include <span>

#include "algebra/reduce_result.h"

namespace xorcle {

namespace {

/// @brief One watch that has to move, and the other watch it is paired with.
struct PropagationTask {
    std::size_t* update_ptr; ///< The watch to move.
    std::size_t* complem_ptr; ///< The other watch, whose row is propagated if no move is possible.
    WatchType type; ///< Which watch is moving, as reported to the observer.
};

} // namespace

PropagationEngine::PropagationEngine(EquationRepr repr, std::size_t num_vars)
    : num_vars_(num_vars),
      working_eqn_(repr, num_vars),
      working_eqn2_(repr, num_vars),
      false_equation_(Equation::eq_false(repr, num_vars)) {}

void PropagationEngine::reconfigure(std::size_t num_vars) {
    num_vars_ = num_vars;
    working_eqn_.resize(num_vars);
    working_eqn2_.resize(num_vars);
    working_eqn_.clear();
    working_eqn2_.clear();

    const std::size_t constant_bit = num_vars;
    false_equation_.resize(num_vars);
    false_equation_.assign_bits(std::span<const std::size_t>(&constant_bit, 1));
}

#ifndef NDEBUG
void PropagationEngine::debug_assert_watch_hint_holds(const Clause& clause, const Trail& trail,
                                                      WatchType hint) {
    working_eqn_.assign_from(clause.row(clause.watched0));
    working_eqn2_.assign_from(clause.row(clause.watched1));
    trail.span().reduce(working_eqn_);
    trail.span().reduce(working_eqn2_);

    const bool watch0_is_zero = working_eqn_.is_eq_true();
    const bool watch1_is_zero = working_eqn2_.is_eq_true();
    const bool sum_is_zero = (working_eqn_ == working_eqn2_);
    const bool watch0_is_one = working_eqn_.is_eq_false();
    const bool watch1_is_one = working_eqn2_.is_eq_false();

    switch (hint) {
        case WatchType::zero:
            assert(watch0_is_zero && "hinted zero trigger is not 0 = 0");
            assert(!watch1_is_one && "hinted zero trigger has companion 0 = 1");
            break;
        case WatchType::one:
            assert(watch1_is_zero && "hinted one trigger is not 0 = 0");
            assert(!watch0_is_one && "hinted one trigger has companion 0 = 1");
            break;
        case WatchType::sum:
            assert(sum_is_zero && "hinted sum trigger is not 0 = 0");
            assert(!watch1_is_one && "hinted sum trigger has companion 0 = 1");
            break;
    }
}
#endif

Equation* PropagationEngine::unit_propagate_one_clause(ClauseId id, Clause& clause,
                                                       const Trail& trail,
                                                       std::optional<WatchType> zero_watch_hint,
                                                       std::size_t& num_watch_updates,
                                                       WatchObserver& observer) {
    assert(clause.cols() == num_vars_ + 1);
    assert(clause.rows() >= 1);
    assert(trail.span().cols() == num_vars_ + 1);

    // A unit clause has nothing to watch: conflict and propagation are the same
    // code path, and the caller tells them apart afterwards.
    if (clause.rows() == 1) {
        assert(!zero_watch_hint.has_value() && "non-unit hint for a unit clause");
        working_eqn_.assign_from(clause.row(0));
        const ReduceResult reduced = trail.span().partial_reduce(working_eqn_);
        if (reduced.kind == ReduceResult::Kind::eq_false) {
            // The clause row reduced to 1, so its negation is 0: already satisfied.
            return nullptr;
        }
        working_eqn_.plus_assign(false_equation_);
        return &working_eqn_;
    }

    assert(clause.watched0 < clause.rows());
    assert(clause.watched1 < clause.rows());
    assert(clause.watched0 != clause.watched1);
    assert(clause.next_watch_search_start < clause.rows());

    // Overwrites `scratch`. `distinct_from` is an already reduced row, so a
    // replacement must be nonzero *and* independent of it modulo the span --
    // that is what keeps the pair legal rather than two names for one row.
    const auto find_replacement_cyclic =
        [&](Equation& scratch, const Equation& distinct_from) -> std::optional<std::size_t> {
        assert(clause.rows() > 1);
        const std::size_t start = clause.next_watch_search_start % clause.rows();
        for (std::size_t step = 0; step < clause.rows(); ++step) {
            const std::size_t i = (start + step) % clause.rows();
            if (i == clause.watched0 || i == clause.watched1) {
                continue;
            }
            scratch.assign_from(clause.row(i));
            const ReduceResult candidate = trail.span().partial_reduce_with_extra(scratch,
                                                                                  distinct_from);
            if (candidate.kind != ReduceResult::Kind::eq_true) {
                clause.next_watch_search_start = (i + 1) % clause.rows();
                return i;
            }
        }
        return std::nullopt;
    };

    std::array<PropagationTask, 2> tasks{};
    std::size_t num_tasks = 0;

    if (zero_watch_hint.has_value()) {
        // The trigger is known, so the reductions are skipped and only the task
        // is built. A sum trigger is normalized here to moving `watched1`.
#ifndef NDEBUG
        debug_assert_watch_hint_holds(clause, trail, *zero_watch_hint);
#endif
        switch (*zero_watch_hint) {
            case WatchType::zero:
                tasks[num_tasks++] = {&clause.watched0, &clause.watched1, WatchType::zero};
                break;
            case WatchType::one:
                tasks[num_tasks++] = {&clause.watched1, &clause.watched0, WatchType::one};
                break;
            case WatchType::sum:
                tasks[num_tasks++] = {&clause.watched1, &clause.watched0, WatchType::one};
                break;
        }
    } else {
        working_eqn_.assign_from(clause.row(clause.watched0));
        const ReduceResult reduced0 = trail.span().partial_reduce(working_eqn_);
        if (reduced0.kind == ReduceResult::Kind::eq_true) {
            tasks[num_tasks++] = {&clause.watched0, &clause.watched1, WatchType::zero};
        }

        // Reducing modulo the span *plus* the reduced first watch folds the sum
        // check in: the result is 0 exactly when the second watch is dead or has
        // become equal to the first. When the first watch reduced to 0 the extra
        // row is 0 too and this degenerates to a plain partial reduction, which
        // is what makes the two-task case come out right.
        working_eqn2_.assign_from(clause.row(clause.watched1));
        const ReduceResult reduced1 = trail.span().partial_reduce_with_extra(working_eqn2_,
                                                                             working_eqn_);
        if (reduced1.kind == ReduceResult::Kind::eq_true) {
            tasks[num_tasks++] = {&clause.watched1, &clause.watched0, WatchType::one};
        }
    }

    for (std::size_t i = 0; i < num_tasks; ++i) {
        const PropagationTask& task = tasks[i];

        // Overwrites the second watch's reduction on purpose: from here only the
        // complement watch's reduction matters.
        working_eqn2_.assign_from(clause.row(*task.complem_ptr));
        const ReduceResult complement = trail.span().partial_reduce(working_eqn2_);

        if (const std::optional<std::size_t> replacement =
                find_replacement_cyclic(working_eqn_, working_eqn2_)) {
            // The clause is updated before the observer is told: the observer
            // reads the clause for the watch's new row while being handed
            // `old_watch` for the old one. Reversing the two corrupts a
            // watchlist keyed on the watched rows.
            const EquationId old_watch = *task.update_ptr;
            *task.update_ptr = *replacement;
            ++num_watch_updates;
            observer.update_with_new_watch(id, old_watch, task.type, trail);
        } else {
            if (complement.kind == ReduceResult::Kind::eq_false) {
                // Complement reduced to 1: satisfied. A second task still runs.
                continue;
            }
            working_eqn2_.plus_assign(false_equation_);
            return &working_eqn2_;
        }
    }
    return nullptr;
}

} // namespace xorcle
