#include "core/trail.h"

#include <cassert>

namespace xorcle {

namespace {

#ifndef NDEBUG
/// @brief The two order maps are inverse permutations of each other, and both
/// agree with the span and the reason list.
void debug_assert_snapshot_valid(const TrailSnapshot& snapshot) {
    const std::size_t trail_rows = snapshot.span().rows();
    assert(snapshot.order_map().size() == trail_rows);
    assert(snapshot.reverse_order_map().size() == trail_rows);
    assert(snapshot.reasons().size() == trail_rows);

    for (std::size_t trail_index = 0; trail_index < trail_rows; ++trail_index) {
        const std::size_t row_index = snapshot.order_map()[trail_index];
        assert(row_index < trail_rows);
        assert(snapshot.reverse_order_map()[row_index] == trail_index);
    }
}
#endif

} // namespace

void TrailSnapshot::learn(const Equation& eqn, std::optional<ClauseId> reason) {
    assert(eqn.size() == span_.cols());

    const std::optional<std::size_t> maybe_inserted_index = span_.add_row_get_index(eqn);
    // A caller that learns an equation already in the span would leave the two
    // order maps shorter than the span, breaking every later trail-order lookup.
    assert(maybe_inserted_index.has_value() && "learned an equation already in the span");

    const std::size_t inserted_index = *maybe_inserted_index;
    reasons_.push_back(reason);

    // Inserting a row shifts every row at or after it one place up, so every
    // trail step that pointed there has to follow.
    for (std::size_t& row_index : order_map_) {
        if (row_index >= inserted_index) {
            ++row_index;
        }
    }
    order_map_.push_back(inserted_index);
    reverse_order_map_.insert(reverse_order_map_.begin() +
                                  static_cast<std::ptrdiff_t>(inserted_index),
                              order_map_.size() - 1);

#ifndef NDEBUG
    debug_assert_snapshot_valid(*this);
#endif
}

Trail::Trail(Algebra backend, std::size_t num_vars) {
    snapshots_.emplace_back(backend, num_vars);
}

void Trail::learn(const Equation& eqn, std::optional<ClauseId> reason) {
    assert(!snapshots_.empty());
    assert(eqn.size() == span().cols());

    if (!reason.has_value()) {
        // A decision opens a new level: a full copy, which is what makes
        // backjumping a truncation rather than an undo log.
        TrailSnapshot new_snapshot = snapshots_.back();
        snapshots_.push_back(std::move(new_snapshot));
    }
    snapshots_.back().learn(eqn, reason);

#ifndef NDEBUG
    debug_assert_snapshot_valid(snapshots_.back());
#endif
}

void Trail::resize(std::size_t num_levels) {
    assert(num_levels >= 1 && "the root snapshot is never discarded");
    assert(num_levels <= snapshots_.size() && "the trail only ever truncates");
    // Not `std::vector::resize`: `TrailSnapshot` has no default constructor,
    // because a snapshot cannot exist without knowing its variable count.
    snapshots_.erase(snapshots_.begin() + static_cast<std::ptrdiff_t>(num_levels),
                     snapshots_.end());
}

} // namespace xorcle
