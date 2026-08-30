#pragma once

/// @file clause_activity.h
/// @brief Reading a clause's activity from outside the decision procedure that keeps it.

#include "core/clause_id.h"

namespace xorcle {

/// @brief A per-clause score another heuristic can order clauses by.
class ClauseActivitySource {
public:
    virtual ~ClauseActivitySource() = default;

    /// @brief The clause's activity, or `0` where it has none.
    virtual double clause_activity(ClauseId id) const = 0;
};

} // namespace xorcle
