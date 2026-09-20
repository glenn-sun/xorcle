#include "core/trail.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

namespace xorcle {

namespace {

/// @brief Where each row lands once the rows flagged in `is_dropped` are popped.
std::vector<std::size_t> row_idxs_after_dropping(const std::vector<std::uint8_t>& is_dropped) {
    std::vector<std::size_t> after(is_dropped.size());
    std::size_t dropped = 0;
    for (std::size_t row_idx = 0; row_idx < is_dropped.size(); ++row_idx) {
        after[row_idx] = row_idx - dropped;
        dropped += is_dropped[row_idx] != 0 ? 1 : 0;
    }
    return after;
}

#ifndef NDEBUG
/// @brief A span and the trail order over it.
struct SpanInTrailOrder {
    RowSpace span;
    std::vector<std::size_t> trail_idx_to_row_idx;
};

/// @brief Insert `eqn` at `trail_idx` by taking every row from there on out of
/// `span` and putting it back after the new one.
SpanInTrailOrder reinsert_above(const RowSpace& span,
                                const std::vector<std::size_t>& trail_idx_to_row_idx,
                                const std::vector<std::size_t>& row_idx_to_trail_idx,
                                const Equation& eqn, std::size_t trail_idx) {
    const std::size_t old_rows = span.rows();
    const std::size_t num_above = old_rows - trail_idx;
    SpanInTrailOrder out{span, std::vector<std::size_t>(old_rows + 1, 0)};

    std::vector<std::uint8_t> is_above(old_rows, 0);
    for (std::size_t t = trail_idx; t < old_rows; ++t) {
        is_above[trail_idx_to_row_idx[t]] = 1;
    }
    // Descending row index, so each pop leaves the rows still to come in place.
    std::vector<std::size_t> pop_order(trail_idx_to_row_idx.begin() +
                                           static_cast<std::ptrdiff_t>(trail_idx),
                                       trail_idx_to_row_idx.end());
    std::sort(pop_order.begin(), pop_order.end(), std::greater<>{});

    std::vector<std::optional<Equation>> above(num_above);
    for (const std::size_t row_idx : pop_order) {
        above[row_idx_to_trail_idx[row_idx] - trail_idx] = out.span.pop_row(row_idx);
    }

    // Row indices in the new trail order, each bumped as a later insert shifts it.
    const std::vector<std::size_t> after_popping = row_idxs_after_dropping(is_above);
    for (std::size_t t = 0; t < trail_idx; ++t) {
        out.trail_idx_to_row_idx[t] = after_popping[trail_idx_to_row_idx[t]];
    }

    const auto insert_row = [&](Equation row, std::size_t at) {
        const std::optional<std::size_t> row_idx = out.span.add_row_get_row_idx(std::move(row));
        assert(row_idx.has_value() && "an inserted row added no rank");
        for (std::size_t t = 0; t < at; ++t) {
            if (out.trail_idx_to_row_idx[t] >= *row_idx) {
                ++out.trail_idx_to_row_idx[t];
            }
        }
        out.trail_idx_to_row_idx[at] = *row_idx;
    };

    insert_row(eqn, trail_idx);
    for (std::size_t i = 0; i < num_above; ++i) {
        insert_row(*std::move(above[i]), trail_idx + 1 + i);
    }
    return out;
}
#endif

} // namespace

Trail::Trail(EquationRepr repr, std::size_t num_vars) : span_(repr, num_vars), level_end_{0} {}

std::size_t Trail::learn(const Equation& eqn, std::optional<ClauseId> reason, std::size_t level) {
    assert(eqn.num_bits() == span_.cols() &&
           "learned equation does not match the trail's column count");
    assert(eqn.repr() == span_.repr() &&
           "learned equation does not match the trail's representation");
    assert((reason.has_value() ? level <= this->level() : level == this->level() + 1) &&
           "a decision opens the next level; a propagation lands at or below the current one");

    if (!reason.has_value()) {
        level_end_.push_back(level_end_.back());
    }

    const std::size_t trail_idx = level_end_[level];
    const std::size_t inserted_trail_idx =
        trail_idx == span_.rows() ? learn_in_order(eqn, std::move(reason), level)
                                  : learn_out_of_order(eqn, std::move(reason), level, trail_idx);

    for (std::size_t l = level; l < level_end_.size(); ++l) {
        ++level_end_[l];
    }

#ifndef NDEBUG
    debug_assert_valid();
#endif
    return inserted_trail_idx;
}

std::size_t Trail::learn_in_order(const Equation& eqn, std::optional<ClauseId> reason,
                                  std::size_t level) {
    const std::optional<std::size_t> maybe_inserted_row_idx = span_.add_row_get_row_idx(eqn);
    assert(maybe_inserted_row_idx.has_value() && "learned an equation already in the span");

    const std::size_t inserted_row_idx = *maybe_inserted_row_idx;
    reasons_.push_back(std::move(reason));
    levels_.push_back(level);

    // Inserting a row shifts every row at or after it one place up.
    for (std::size_t& row_idx : trail_idx_to_row_idx_) {
        if (row_idx >= inserted_row_idx) {
            ++row_idx;
        }
    }
    trail_idx_to_row_idx_.push_back(inserted_row_idx);
    row_idx_to_trail_idx_.insert(row_idx_to_trail_idx_.begin() +
                                     static_cast<std::ptrdiff_t>(inserted_row_idx),
                                 trail_idx_to_row_idx_.size() - 1);
    return trail_idx_to_row_idx_.size() - 1;
}

std::size_t Trail::learn_out_of_order(const Equation& eqn, std::optional<ClauseId> reason,
                                      std::size_t level, std::size_t trail_idx) {
#ifndef NDEBUG
    const Trail before = *this;
#endif
    // The elements from `trail_idx` on each move one place up in trail order.
    for (std::size_t& t : row_idx_to_trail_idx_) {
        t += t >= trail_idx ? 1 : 0;
    }

    // The row of the element at `row_trail_idx`, which is the one element the
    // span is currently missing.
    Equation row = eqn;
    std::size_t row_trail_idx = trail_idx;
    while (true) {
        const ReduceResult reduced =
            span_.partial_reduce_below(row, row_idx_to_trail_idx_, row_trail_idx);
        assert(reduced != ReduceResult::eq_true() &&
               "a learned row was already in the span of the elements before it");
        const std::size_t leading_col_idx = reduced.is_leading() ? reduced.bit : span_.cols() - 1;

        // A row of a later element holding that column loses it to this row.
        std::optional<Equation> displaced;
        std::size_t displaced_trail_idx = 0;
        if (span_.pivot_col_idxs().test(leading_col_idx)) {
            const std::size_t displaced_row_idx =
                span_.pivot_col_idxs().count_below(leading_col_idx);
            displaced_trail_idx = row_idx_to_trail_idx_[displaced_row_idx];
            assert(displaced_trail_idx > row_trail_idx &&
                   "the reduction stopped on the pivot of an earlier element");
            displaced = span_.pop_row(displaced_row_idx);
            row_idx_to_trail_idx_.erase(row_idx_to_trail_idx_.begin() +
                                        static_cast<std::ptrdiff_t>(displaced_row_idx));
        }

        const std::optional<std::size_t> inserted_row_idx =
            span_.add_row_get_row_idx(std::move(row));
        assert(inserted_row_idx.has_value() && "an inserted row added no rank");
        row_idx_to_trail_idx_.insert(
            row_idx_to_trail_idx_.begin() + static_cast<std::ptrdiff_t>(*inserted_row_idx),
            row_trail_idx);

        if (!displaced.has_value()) {
            break;
        }
        row = *std::move(displaced);
        row.plus_assign(span_.row(*inserted_row_idx));
        row_trail_idx = displaced_trail_idx;
    }

    reasons_.insert(reasons_.begin() + static_cast<std::ptrdiff_t>(trail_idx), std::move(reason));
    levels_.insert(levels_.begin() + static_cast<std::ptrdiff_t>(trail_idx), level);
    rebuild_trail_idx_to_row_idx();

#ifndef NDEBUG
    debug_assert_matches_reinsertion(before, eqn, trail_idx);
#endif
    return trail_idx;
}

void Trail::resize(std::size_t num_levels) {
    assert(num_levels >= 1 && "the root level is never discarded");
    assert(num_levels <= level_end_.size() && "the trail only ever truncates");

    const std::size_t kept = level_end_[num_levels - 1];
    const std::size_t old_rows = span_.rows();
    level_end_.resize(num_levels);
    if (kept == old_rows) {
        return;
    }

    std::vector<std::uint8_t> is_dropped(old_rows, 0);
    for (std::size_t t = kept; t < old_rows; ++t) {
        is_dropped[trail_idx_to_row_idx_[t]] = 1;
    }
    std::vector<std::size_t> pop_order(trail_idx_to_row_idx_.begin() +
                                           static_cast<std::ptrdiff_t>(kept),
                                       trail_idx_to_row_idx_.end());
    std::sort(pop_order.begin(), pop_order.end(), std::greater<>{});
    for (const std::size_t row_idx : pop_order) {
        span_.pop_row(row_idx);
    }

    trail_idx_to_row_idx_.resize(kept);
    reasons_.resize(kept);
    levels_.resize(kept);

    const std::vector<std::size_t> after_popping = row_idxs_after_dropping(is_dropped);
    for (std::size_t& row_idx : trail_idx_to_row_idx_) {
        row_idx = after_popping[row_idx];
    }
    rebuild_row_idx_to_trail_idx();

#ifndef NDEBUG
    debug_assert_valid();
#endif
}

void Trail::rebuild_row_idx_to_trail_idx() {
    row_idx_to_trail_idx_.assign(trail_idx_to_row_idx_.size(), 0);
    for (std::size_t t = 0; t < trail_idx_to_row_idx_.size(); ++t) {
        row_idx_to_trail_idx_[trail_idx_to_row_idx_[t]] = t;
    }
}

void Trail::rebuild_trail_idx_to_row_idx() {
    trail_idx_to_row_idx_.assign(row_idx_to_trail_idx_.size(), 0);
    for (std::size_t row_idx = 0; row_idx < row_idx_to_trail_idx_.size(); ++row_idx) {
        trail_idx_to_row_idx_[row_idx_to_trail_idx_[row_idx]] = row_idx;
    }
}

#ifndef NDEBUG
void Trail::debug_assert_matches_reinsertion(const Trail& before, const Equation& eqn,
                                             std::size_t trail_idx) const {
    const SpanInTrailOrder reference =
        reinsert_above(before.span_, before.trail_idx_to_row_idx_, before.row_idx_to_trail_idx_,
                       eqn, trail_idx);
    assert(reference.trail_idx_to_row_idx == trail_idx_to_row_idx_ &&
           "the chain insert put a row where reinsertion would not");
    for (std::size_t row_idx = 0; row_idx < span_.rows(); ++row_idx) {
        assert(reference.trail_idx_to_row_idx[row_idx_to_trail_idx_[row_idx]] == row_idx &&
               "the chain insert left the order maps disagreeing with reinsertion");
        assert(reference.span.row(row_idx) == span_.row(row_idx) &&
               "the chain insert stored a row reinsertion would not");
    }
}

void Trail::debug_assert_valid() const {
    const std::size_t trail_rows = span_.rows();
    assert(trail_idx_to_row_idx_.size() == trail_rows);
    assert(row_idx_to_trail_idx_.size() == trail_rows);
    assert(reasons_.size() == trail_rows);
    assert(levels_.size() == trail_rows);

    for (std::size_t trail_idx = 0; trail_idx < trail_rows; ++trail_idx) {
        const std::size_t row_idx = trail_idx_to_row_idx_[trail_idx];
        assert(row_idx < trail_rows);
        assert(row_idx_to_trail_idx_[row_idx] == trail_idx);
        assert(trail_idx == 0 || levels_[trail_idx - 1] <= levels_[trail_idx]);
        assert(levels_[trail_idx] < level_end_.size());
        assert(reasons_[trail_idx].has_value() ||
               (levels_[trail_idx] > 0 && level_end_[levels_[trail_idx] - 1] == trail_idx));
    }

    assert(level_end_.back() == trail_rows);
    for (std::size_t level = 0; level < level_end_.size(); ++level) {
        assert(level == 0 || level_end_[level - 1] <= level_end_[level]);
        assert(level_end_[level] == trail_rows || levels_[level_end_[level]] > level);
        assert(level_end_[level] == 0 || levels_[level_end_[level] - 1] <= level);
    }
}
#endif

} // namespace xorcle
