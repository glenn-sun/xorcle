#pragma once

#include <cstddef>

#include "algebra/equation.h"
#include "algebra/reduce_result.h"
#include "core/clause_id.h"

namespace xorcle {

class Trail;

/// @brief Why a watched pair of a clause stopped being a legal watch pair.
///
/// A clause with more than one row watches two distinct rows of its linear
/// negation, maintaining, modulo the trail span, that the first is not `0 = 0`
/// and the second is independent of it.
enum class WatchType {
    zero, ///< The first watch reduced to `0 = 0`.
    one ///< The second watch reduced to `0 = 0`.
};

/// @brief The rows of a clause's linear negation that propagation watches.
struct ClauseWatches {
    ClauseRowIdx watched0 = 0;
    ClauseRowIdx watched1 = 1;

    /// @brief Where the next cyclic search for a replacement watch starts.
    ClauseRowIdx next_watch_search_start = 0;

    /// @brief The first two rows, or row 0 twice for a unit clause: a legal pair
    /// against an empty trail, where no stored row is `0 = 0` and any two are independent.
    static ClauseWatches first_rows(std::size_t width) {
        return {.watched0 = 0, .watched1 = width <= 1 ? 0U : 1U, .next_watch_search_start = 0};
    }
};

/// @brief How a reduction a handler holds relates to the watch's partial
/// reduction by the trail span.
enum class HeldReduction {
    none, ///< The handler holds nothing.
    congruent, ///< Leads as it does and differs from it by an element of the span.
    exact ///< Equal to it.
};

/// @brief The partial reductions of a clause's watches that a watch move computed.
struct WatchReductions {
    /// @brief The new first watch reduced by the trail span; null when the
    /// second watch moved.
    const Equation* first = nullptr;
    ReduceResult first_result = ReduceResult::eq_true();

    /// @brief The second watch reduced by the trail span and the first watch's
    /// reduction, or, computed from a congruent reduction, congruent to that.
    const Equation* second = nullptr;
    ReduceResult second_result = ReduceResult::eq_true();
};

/// @brief Receives the watch movements a propagation step performs, and may
/// hold reductions the step would otherwise repeat.
class WatchUpdateHandler {
public:
    virtual ~WatchUpdateHandler() = default;

    /// @brief Copy into `reduced` what the handler holds of the watch a trigger of
    /// `type` leaves standing, reduced by the trail span, and set `result` to what
    /// it reduced to.
    /// @pre The trigger holds on clause `id`, and `reduced` has the trail rows' bit count.
    virtual HeldReduction copy_reduced_companion(ClauseId id, WatchType type, Equation& reduced,
                                                 ReduceResult& result) = 0;

    /// @brief Report that a watch of clause `id` moved, called *after* its
    /// `ClauseWatches` are updated.
    /// @param reductions Valid only during the call.
    virtual void update_with_new_watch(ClauseId id, ClauseRowIdx old_watch_row_idx, WatchType type,
                                       const Trail& trail, const WatchReductions& reductions) = 0;
};

} // namespace xorcle
