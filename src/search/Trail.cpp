#include "search/Trail.h"

#include <cassert>
#include <stdexcept>

namespace search {

namespace {

#ifndef NDEBUG
void debug_assert_snapshot_valid(const TrailSnapshot& snapshot) {
    const std::size_t trail_rows = snapshot.span.rows();
    assert(snapshot.order_map.size() == trail_rows);
    assert(snapshot.reverse_order_map.size() == trail_rows);
    assert(snapshot.reasons.size() == trail_rows);

    for (std::size_t trail_index = 0; trail_index < trail_rows; ++trail_index) {
        const std::size_t row_index = snapshot.order_map[trail_index];
        assert(row_index < trail_rows);
        assert(snapshot.reverse_order_map[row_index] == trail_index);
    }
}
#endif

} // namespace

void Trail::learn(const algebra::Equation& eqn, std::optional<heuristics::ClauseId> reason) {
#ifndef NDEBUG
    assert(!empty());
    assert(eqn.size() == span().cols());
#endif

    if (!reason.has_value()) {
        TrailSnapshot new_snapshot = back();
        push_back(new_snapshot);
    }
    back().learn(eqn, reason);

#ifndef NDEBUG
    debug_assert_snapshot_valid(back());
#endif
}

void TrailSnapshot::learn(const algebra::Equation& eqn, std::optional<heuristics::ClauseId> reason) {
#ifndef NDEBUG
    assert(eqn.size() == span.cols());
#endif

    const std::optional<std::size_t> maybe_inserted_index = span.add_row_get_index(eqn);
#ifndef NDEBUG
    assert(maybe_inserted_index.has_value());
#endif

    const std::size_t inserted_index = *maybe_inserted_index;
    reasons.push_back(reason);

    for (std::size_t& row_index : order_map) {
        if (row_index >= inserted_index) {
            ++row_index;
        }
    }
    order_map.push_back(inserted_index);
    reverse_order_map.insert(reverse_order_map.begin() + inserted_index, order_map.size() - 1);

#ifndef NDEBUG
    debug_assert_snapshot_valid(*this);
#endif
}
} // namespace search
