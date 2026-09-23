#include "core/propagation.h"

#include <array>
#include <cassert>
#include <optional>
#include <utility>

#include "algebra/reduce_result.h"

namespace xorcle {

namespace {

/// @brief One watch that has to move, and the other watch it is paired with.
struct PropagationTask {
    std::size_t* update_ptr;
    std::size_t* complement_ptr; ///< Its row is propagated if no move is possible.
    WatchType type;
};

/// @brief The partial reduction of a row whose leading column the span does not pivot.
ReduceResult unreduced_result(std::size_t leading_col_idx, std::size_t num_vars) noexcept {
    return leading_col_idx == num_vars ? ReduceResult::eq_false()
                                       : ReduceResult::leading(leading_col_idx);
}

} // namespace

Propagator::Propagator(EquationRepr repr, std::size_t num_vars)
    : num_vars_(num_vars),
      working_eqn_(repr, num_vars),
      working_eqn2_(repr, num_vars),
      used_rows_eqn_(repr, num_vars),
      used_rows_eqn2_(repr, num_vars),
      false_equation_(Equation::eq_false(repr, num_vars)) {}

void Propagator::reconfigure(std::size_t num_vars) {
    num_vars_ = num_vars;
    working_eqn_.resize(num_vars);
    working_eqn2_.resize(num_vars);
    working_eqn_.set_eq_true();
    working_eqn2_.set_eq_true();
    used_rows_eqn_.resize(num_vars);
    used_rows_eqn_.set_eq_true();
    used_rows_eqn2_.resize(num_vars);
    used_rows_eqn2_.set_eq_true();

    false_equation_.resize(num_vars);
    false_equation_.set_eq_false();
}

#ifndef NDEBUG
void Propagator::debug_assert_watch_hint_holds(const Clause& clause, const Trail& trail,
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
        case WatchType::sum:
            // Both move the second watch, so either holds for both.
            assert((watch1_is_zero || sum_is_zero) &&
                   "hinted one or sum trigger: the second watch is not in the span plus the first");
            assert(!watch0_is_one && "hinted one or sum trigger has companion 0 = 1");
            break;
    }
}
#endif

Equation* Propagator::unit_propagate_one_clause(ClauseId id, Clause& clause, const Trail& trail,
                                                std::optional<WatchType> trigger_hint,
                                                std::size_t& num_watch_updates,
                                                WatchUpdateHandler& handler,
                                                IndexSet* used_row_idxs) {
    assert(clause.cols() == num_vars_ + 1 &&
           "clause column count does not match the propagator's variable count");
    assert(trail.span().cols() == num_vars_ + 1 &&
           "trail span column count does not match the propagator's variable count");
    assert(clause.repr() == trail.repr() &&
           "clause and trail use different representations");
    assert(clause.rows() >= 1 && "propagating from a clause with no rows");

    found_satisfied_ = false;

    // A unit clause has nothing to watch.
    if (clause.rows() == 1) {
        assert(!trigger_hint.has_value() && "non-unit hint for a unit clause");
        working_eqn_.assign_from(clause.row(0));
        const ReduceResult reduced = trail.span().partial_reduce(working_eqn_);
        if (reduced.kind == ReduceResult::Kind::eq_false) {
            // Reduced to 1, so its negation is 0: already satisfied.
            found_satisfied_ = true;
            satisfying_row_idx_ = 0;
            return nullptr;
        }
        working_eqn_.plus_assign(false_equation_);
        propagating_row_idx_ = 0;
        if (used_row_idxs != nullptr) {
            collect_used_rows(clause, trail, propagating_row_idx_,
                              reduced.kind == ReduceResult::Kind::eq_true, *used_row_idxs);
        }
        return &working_eqn_;
    }

    assert(clause.watched0 < clause.rows() && clause.watched1 < clause.rows() &&
           "a watched row index is out of range");
    assert(clause.watched0 != clause.watched1 && "both watches name the same row");
    assert(clause.next_watch_search_start < clause.rows() &&
           "the watch search start is out of range");

    // Overwrites `scratch`. `distinct_from` is already reduced, so a replacement
    // must be nonzero *and* independent of it modulo the span.
    const auto find_replacement_cyclic =
        [&](Equation& scratch, const Equation& distinct_from) -> std::optional<std::size_t> {
        assert(clause.rows() > 1 && "cyclic watch search on a unit clause");
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
    // Without a hint, the first watch's partial reduction, held in `working_eqn_`
    // if a reduction step was due and otherwise the row itself.
    ReduceResult reduced0 = ReduceResult::eq_true();
    bool reduced0_in_working_eqn = false;

    if (trigger_hint.has_value()) {
        // A sum trigger is normalized here to moving `watched1`.
#ifndef NDEBUG
        debug_assert_watch_hint_holds(clause, trail, *trigger_hint);
#endif
        switch (*trigger_hint) {
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
        // A row is copied into scratch only when a reduction step is due; until
        // then it is its own partial reduction.
        const IndexSet& span_pivot_col_idxs = trail.span().pivot_col_idxs();

        const std::size_t leading_col_idx0 = clause.pivot_col_idx(clause.watched0);
        if (span_pivot_col_idxs.test(leading_col_idx0)) {
            working_eqn_.assign_from(clause.row(clause.watched0));
            reduced0 = trail.span().partial_reduce(working_eqn_, leading_col_idx0);
            reduced0_in_working_eqn = true;
        } else {
            reduced0 = unreduced_result(leading_col_idx0, num_vars_);
        }
        if (reduced0.kind == ReduceResult::Kind::eq_false) {
            // This row's equation holds, so the clause does, and nothing it could
            // imply is new. Free to notice: the reduction was needed anyway. The
            // second watch cannot be read this way, being reduced against the
            // span augmented with the first.
            found_satisfied_ = true;
            satisfying_row_idx_ = clause.watched0;
            return nullptr;
        }
        if (reduced0.kind == ReduceResult::Kind::eq_true) {
            tasks[num_tasks++] = {&clause.watched0, &clause.watched1, WatchType::zero};
        }

        // Reducing modulo the span *plus* the reduced first watch folds the sum
        // check in: the result is 0 exactly when the second watch is dead or has
        // become equal to the first.
        const std::optional<std::size_t> extra_leading_col_idx =
            reduced0.is_leading() ? std::optional<std::size_t>(reduced0.bit) : std::nullopt;
        const std::size_t leading_col_idx1 = clause.pivot_col_idx(clause.watched1);
        if (span_pivot_col_idxs.test(leading_col_idx1) ||
            extra_leading_col_idx == leading_col_idx1) {
            working_eqn2_.assign_from(clause.row(clause.watched1));
            const ReduceResult reduced1 = trail.span().partial_reduce_with_extra(
                working_eqn2_, leading_col_idx1,
                reduced0_in_working_eqn ? working_eqn_ : clause.row(clause.watched0),
                extra_leading_col_idx);
            if (reduced1.kind == ReduceResult::Kind::eq_true) {
                tasks[num_tasks++] = {&clause.watched1, &clause.watched0, WatchType::one};
            }
        }
    }

    for (std::size_t i = 0; i < num_tasks; ++i) {
        const PropagationTask& task = tasks[i];

        const Equation* complement_eqn = &working_eqn2_;
        ReduceResult complement;
        if (i == 0 && !trigger_hint.has_value() && task.type == WatchType::one) {
            // The complement is the first watch, whose reduction is still at hand.
            complement = reduced0;
            if (reduced0_in_working_eqn) {
                std::swap(working_eqn_, working_eqn2_);
            } else {
                complement_eqn = &clause.row(clause.watched0);
            }
        } else {
            // Overwrites the second watch's reduction on purpose.
            working_eqn2_.assign_from(clause.row(*task.complement_ptr));
            complement = trail.span().partial_reduce(working_eqn2_);
        }

        if (const std::optional<std::size_t> replacement =
                find_replacement_cyclic(working_eqn_, *complement_eqn)) {
            const ClauseRowIdx old_watch = *task.update_ptr;
            *task.update_ptr = *replacement;
            ++num_watch_updates;
            handler.update_with_new_watch(id, old_watch, task.type, trail);
        } else {
            if (complement.kind == ReduceResult::Kind::eq_false) {
                // Complement reduced to 1: satisfied. A second task still runs.
                found_satisfied_ = true;
                satisfying_row_idx_ = *task.complement_ptr;
                continue;
            }
            if (complement_eqn != &working_eqn2_) {
                working_eqn2_.assign_from(*complement_eqn);
            }
            working_eqn2_.plus_assign(false_equation_);
            propagating_row_idx_ = *task.complement_ptr;
            if (used_row_idxs != nullptr) {
                collect_used_rows(clause, trail, propagating_row_idx_,
                                  complement.kind == ReduceResult::Kind::eq_true, *used_row_idxs);
            }
            return &working_eqn2_;
        }
    }
    return nullptr;
}

void Propagator::collect_used_rows(const Clause& clause, const Trail& trail,
                                   std::size_t propagating_row_idx,
                                   bool propagating_row_is_falsified, IndexSet& used_row_idxs) {
    used_row_idxs.resize(trail.span().rows());

    const Equation& propagating_row = clause.row(propagating_row_idx);
    if (propagating_row_is_falsified) {
        // The clause is falsified too, and the equation it implies is `0 = 1`:
        // every row takes part.
        used_rows_eqn2_.assign_from(propagating_row);
        trail.span().reduce_get_coefficients(used_rows_eqn2_, used_row_idxs);
        assert(used_rows_eqn2_.is_eq_true() && "the propagating row was not falsified after all");
    }

    for (std::size_t row_idx = 0; row_idx < clause.rows(); ++row_idx) {
        if (row_idx == propagating_row_idx) {
            continue;
        }
        used_rows_eqn_.assign_from(clause.row(row_idx));
        trail.span().reduce_get_coefficients(used_rows_eqn_, row_coefficients_);
        if (!used_rows_eqn_.is_eq_true()) {
            // What is left is the propagating row's own residue, so the trail
            // owes this row and that one only their sum.
            used_rows_eqn_.assign_from(clause.row(row_idx));
            used_rows_eqn_.plus_assign(propagating_row);
            trail.span().reduce_get_coefficients(used_rows_eqn_, row_coefficients_);
            assert(used_rows_eqn_.is_eq_true() && "the sum left the span it was reduced by");
        }
        used_row_idxs.or_with(row_coefficients_);
    }
}

} // namespace xorcle
