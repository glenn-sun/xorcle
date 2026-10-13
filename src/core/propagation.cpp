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

/// @brief The leading column of an equation partially reduced to `reduced`, empty for `0 = 0`.
std::optional<std::size_t> leading_col_idx_of(const ReduceResult& reduced,
                                              std::size_t num_vars) noexcept {
    switch (reduced.kind) {
        case ReduceResult::Kind::eq_true:
            return std::nullopt;
        case ReduceResult::Kind::eq_false:
            return num_vars;
        case ReduceResult::Kind::leading:
            return reduced.bit;
    }
    return std::nullopt;
}

} // namespace

Propagator::Propagator(EquationRepr repr, std::size_t num_vars)
    : num_vars_(num_vars),
      working_eqn_(repr, num_vars),
      working_eqn2_(repr, num_vars),
      working_eqn3_(repr, num_vars),
      used_rows_eqn_(repr, num_vars),
      used_rows_eqn2_(repr, num_vars),
      false_equation_(Equation::eq_false(repr, num_vars)) {}

void Propagator::reconfigure(std::size_t num_vars) {
    num_vars_ = num_vars;
    working_eqn_.resize(num_vars);
    working_eqn2_.resize(num_vars);
    working_eqn3_.resize(num_vars);
    working_eqn_.set_eq_true();
    working_eqn2_.set_eq_true();
    working_eqn3_.set_eq_true();
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
            assert((watch1_is_zero || sum_is_zero) &&
                   "hinted one trigger: the second watch is not in the span plus the first");
            assert(!watch0_is_one && "hinted one trigger has companion 0 = 1");
            break;
    }
}

void Propagator::debug_assert_is_held_reduction(const Equation& row, const Trail& trail,
                                                const Equation& reduced,
                                                const ReduceResult& result, HeldReduction held) {
    Equation expected = row;
    const ReduceResult expected_result = trail.span().partial_reduce(expected);
    assert(expected_result == result && "the handler's reduction is misclassified");
    assert(reduced.classify() == result &&
           "the handler's reduction does not lead where it says");
    if (held == HeldReduction::exact) {
        assert(expected == reduced && "the handler's reduction is not the row's partial reduction");
    } else {
        expected.plus_assign(reduced);
        trail.span().reduce(expected);
        assert(expected.is_eq_true() && "the handler's reduction left the row's coset of the span");
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
            // Reduced to `0 = 1`, so its negation is `0 = 0`: already satisfied.
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

    const IndexSet& span_pivot_col_idxs = trail.span().pivot_col_idxs();
    const std::size_t num_rows = clause.rows();

    // What the search below found: the replacement's partial reduction by the
    // span, and, where that led as `distinct_from` does, the same reduced on by
    // `distinct_from` and the span; null where it did not.
    const Equation* replacement_eqn = nullptr;
    ReduceResult replacement_result = ReduceResult::eq_true();
    const Equation* continued_eqn = nullptr;
    ReduceResult continued_result = ReduceResult::eq_true();

    // `distinct_from` is already reduced and leads in `distinct_from_leading_col_idx`,
    // so a replacement must be nonzero *and* independent of it modulo the span.
    // Overwrites `working_eqn_`, and `working_eqn3_` if `keep_replacement_eqn`,
    // which leaves `replacement_eqn` standing beside `continued_eqn`.
    const auto find_replacement_cyclic =
        [&](const Equation& distinct_from,
            std::optional<std::size_t> distinct_from_leading_col_idx,
            bool keep_replacement_eqn) -> std::optional<std::size_t> {
        assert(num_rows > 1 && "cyclic watch search on a unit clause");
        std::size_t i = clause.next_watch_search_start % num_rows;
        for (std::size_t step = 0; step < num_rows; ++step, i = i + 1 == num_rows ? 0 : i + 1) {
            if (i == clause.watched0 || i == clause.watched1) {
                continue;
            }
            // A row no reduction step is due on is its own partial reduction,
            // and no stored row is `0 = 0`.
            const std::size_t leading_col_idx = clause.pivot_col_idx(i);
            replacement_eqn = &clause.row(i);
            replacement_result = unreduced_result(leading_col_idx, num_vars_);
            if (span_pivot_col_idxs.test(leading_col_idx)) {
                working_eqn_.assign_from(clause.row(i));
                replacement_result = trail.span().partial_reduce(working_eqn_, leading_col_idx);
                if (replacement_result.kind == ReduceResult::Kind::eq_true) {
                    continue;
                }
                replacement_eqn = &working_eqn_;
            }
            continued_eqn = nullptr;
            if (leading_col_idx_of(replacement_result, num_vars_) ==
                distinct_from_leading_col_idx) {
                Equation& continued = replacement_eqn == &working_eqn_ && keep_replacement_eqn
                                          ? working_eqn3_
                                          : working_eqn_;
                if (&continued != replacement_eqn) {
                    continued.assign_from(*replacement_eqn);
                }
                continued_result = trail.span().partial_reduce_with_extra(
                    continued, *distinct_from_leading_col_idx, distinct_from,
                    distinct_from_leading_col_idx);
                if (continued_result.kind == ReduceResult::Kind::eq_true) {
                    continue;
                }
                continued_eqn = &continued;
            }
            clause.next_watch_search_start = i + 1 == num_rows ? 0 : i + 1;
            return i;
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
        }
    } else {
        // A row is copied into scratch only when a reduction step is due; until
        // then it is its own partial reduction.
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
        // check in: the result is `0 = 0` exactly when the second watch is dead or has
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
        // Overwrites the second watch's reduction on purpose.
        const auto reduce_complement = [&] {
            const std::size_t complement_row_idx = *task.complement_ptr;
            const std::size_t leading_col_idx = clause.pivot_col_idx(complement_row_idx);
            if (span_pivot_col_idxs.test(leading_col_idx)) {
                working_eqn2_.assign_from(clause.row(complement_row_idx));
                complement = trail.span().partial_reduce(working_eqn2_, leading_col_idx);
                complement_eqn = &working_eqn2_;
            } else {
                complement = unreduced_result(leading_col_idx, num_vars_);
                complement_eqn = &clause.row(complement_row_idx);
            }
        };
        // A congruent complement decides every replacement as the exact one does,
        // but is not the equation to propagate.
        HeldReduction held = HeldReduction::none;
        if (i == 0 && !trigger_hint.has_value() && task.type == WatchType::one) {
            // The complement is the first watch, whose reduction is still at hand.
            complement = reduced0;
            if (reduced0_in_working_eqn) {
                std::swap(working_eqn_, working_eqn2_);
            } else {
                complement_eqn = &clause.row(clause.watched0);
            }
        } else {
            if (trigger_hint.has_value()) {
                held = handler.copy_reduced_companion(id, task.type, working_eqn2_, complement);
            }
            if (held == HeldReduction::none) {
                reduce_complement();
            }
#ifndef NDEBUG
            else {
                debug_assert_is_held_reduction(clause.row(*task.complement_ptr), trail,
                                               working_eqn2_, complement, held);
            }
#endif
        }

        if (const std::optional<std::size_t> replacement =
                find_replacement_cyclic(*complement_eqn, leading_col_idx_of(complement, num_vars_),
                                        task.type == WatchType::zero)) {
            const ClauseRowIdx old_watch_row_idx = *task.update_ptr;
            *task.update_ptr = *replacement;
            ++num_watch_updates;
            const bool continued = continued_eqn != nullptr;
            WatchReductions reductions;
            if (task.type == WatchType::zero) {
                // The second watch stands, and of two reductions that lead alike
                // the sum is either one reduced by the other.
                reductions.first = replacement_eqn;
                reductions.first_result = replacement_result;
                reductions.second = continued ? continued_eqn : complement_eqn;
                reductions.second_result = continued ? continued_result : complement;
            } else {
                reductions.second = continued ? continued_eqn : replacement_eqn;
                reductions.second_result = continued ? continued_result : replacement_result;
            }
            handler.update_with_new_watch(id, old_watch_row_idx, task.type, trail, reductions);
        } else {
            if (complement.kind == ReduceResult::Kind::eq_false) {
                // Complement reduced to `0 = 1`: satisfied. A second task still runs.
                found_satisfied_ = true;
                satisfying_row_idx_ = *task.complement_ptr;
                continue;
            }
            if (held == HeldReduction::congruent) {
                reduce_complement();
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
