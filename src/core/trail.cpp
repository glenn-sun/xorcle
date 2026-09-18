#include "core/trail.h"

#include <cassert>

namespace xorcle {

namespace {

#ifndef NDEBUG
void debug_assert_snapshot_valid(const TrailSnapshot& snapshot) {
    const std::size_t trail_rows = snapshot.span().rows();
    assert(snapshot.trail_idx_to_row_idx().size() == trail_rows);
    assert(snapshot.row_idx_to_trail_idx().size() == trail_rows);
    assert(snapshot.reasons().size() == trail_rows);

    for (std::size_t trail_idx = 0; trail_idx < trail_rows; ++trail_idx) {
        const std::size_t row_idx = snapshot.trail_idx_to_row_idx()[trail_idx];
        assert(row_idx < trail_rows);
        assert(snapshot.row_idx_to_trail_idx()[row_idx] == trail_idx);
    }
}
#endif

} // namespace

void TrailSnapshot::learn(const Equation& eqn, std::optional<ClauseId> reason) {
    assert(eqn.num_bits() == span_.cols() &&
           "learned equation does not match the snapshot's column count");
    assert(eqn.repr() == span_.repr() &&
           "learned equation does not match the snapshot's representation");

    const std::optional<std::size_t> maybe_inserted_row_idx = span_.add_row_get_row_idx(eqn);
    assert(maybe_inserted_row_idx.has_value() && "learned an equation already in the span");

    const std::size_t inserted_row_idx = *maybe_inserted_row_idx;
    reasons_.push_back(reason);

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

#ifndef NDEBUG
    debug_assert_snapshot_valid(*this);
#endif
}

Trail::Trail(EquationRepr repr, std::size_t num_vars) {
    snapshots_.emplace_back(repr, num_vars);
}

void Trail::learn(const Equation& eqn, std::optional<ClauseId> reason) {
    assert(!snapshots_.empty() && "a trail always holds at least the root snapshot");

    if (!reason.has_value()) {
        // A decision opens a new level.
        TrailSnapshot new_snapshot = snapshots_.back();
        snapshots_.push_back(std::move(new_snapshot));
    }
    snapshots_.back().learn(eqn, reason);
}

void Trail::resize(std::size_t num_levels) {
    assert(num_levels >= 1 && "the root snapshot is never discarded");
    assert(num_levels <= snapshots_.size() && "the trail only ever truncates");
    snapshots_.erase(snapshots_.begin() + static_cast<std::ptrdiff_t>(num_levels),
                     snapshots_.end());
}

} // namespace xorcle
