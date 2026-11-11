#include "core/saved_trail.h"

#include <algorithm>
#include <cassert>

namespace xorcle {

void SavedTrail::save(const Trail& trail, std::span<const std::size_t> trail_stamps,
                      std::size_t num_levels, std::size_t end) {
    const std::size_t base = trail.rows_at_level(0);
    if (rows.size() < end - base) {
        rows.resize(end - base);
    }
    for (std::size_t trail_idx = base; trail_idx < end; ++trail_idx) {
        Equation& row = rows[trail_idx - base];
        if (row.num_bits() == trail.span().cols()) {
            row.assign_from(trail.row(trail_idx));
        } else {
            row = trail.row(trail_idx);
        }
    }
    level_ends.resize(num_levels);
    for (std::size_t level = 1; level <= num_levels; ++level) {
        level_ends[level - 1] = std::min(trail.rows_at_level(level), end) - base;
    }
    const std::span<const std::size_t> stamps = trail_stamps.first(num_levels + 1);
    assert(std::ranges::find(stamps, unshared_level_stamp) == stamps.end() &&
           "a trail level has the stamp no level has");
    level_stamps.assign(stamps.begin(), stamps.end());
    root_rows = base;
}

void SavedTrail::save_decisions(const Trail& trail, std::span<const std::size_t> trail_stamps,
                                const std::size_t num_levels) {
    rows.resize(num_levels, Equation(trail.repr(), trail.span().cols() - 1));
    level_ends.resize(num_levels);
    for (std::size_t level = 1; level <= num_levels; ++level) {
        const std::size_t trail_idx = trail.rows_at_level(level - 1);
        assert(!trail.reasons()[trail_idx].has_value() && "a level opens with a propagated row");
        Equation& row = rows[level - 1];
        if (row.num_bits() == trail.span().cols()) {
            row.assign_from(trail.row(trail_idx));
        } else {
            row = trail.row(trail_idx);
        }
        level_ends[level - 1] = level;
    }
    const std::span<const std::size_t> stamps = trail_stamps.first(num_levels + 1);
    level_stamps.assign(stamps.begin(), stamps.end());
    root_rows = trail.rows_at_level(0);
}

void SavedTrail::substitute(const RowSpace& root_span) {
    assert(root_span.rows() >= root_rows && "the root lost rows a saved trail had");
    for (std::size_t row_idx = 0; row_idx < num_rows(); ++row_idx) {
        root_span.reduce(rows[row_idx]);
    }
    std::ranges::fill(level_stamps, unshared_level_stamp);
    root_rows = 0;
}

void SavedTrail::renumber(const CompactionTransform& transform) {
    assert(root_rows == 0 && "a renumbering follows a substitution");
    for (std::size_t row_idx = 0; row_idx < num_rows(); ++row_idx) {
        rows[row_idx] = rows[row_idx].compact(transform);
    }
    rows.resize(num_rows());
}

} // namespace xorcle
