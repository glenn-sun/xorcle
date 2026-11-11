#pragma once

/// @file equivalent_clause_table.h
/// @brief Finding the kept clause whose rows are those of a given clause.

#include <cstddef>
#include <optional>
#include <unordered_map>
#include <vector>

#include "algebra/row_echelon_matrix.h"
#include "core/clause_id.h"

namespace xorcle {

/// @brief The clauses entered, by a hash of their pivot columns and constants.
///
/// Two clauses in reduced row-echelon form have one span exactly when they have
/// the same rows, which decides between clauses of one key. A key is a clause's
/// `rows_hash()` over names of its variables that no renumbering changes, so the
/// entries outlive a renumbering.
class EquivalentClauseTable {
public:
    /// @brief An entered clause with the rows of @p clause, if there is one.
    /// @pre `clauses[id]` is each entered clause, with the rows its key was taken of.
    std::optional<ClauseId> find(const Clause& clause, std::size_t key,
                                 const std::vector<Clause>& clauses) const;

    /// @brief The entered clause with the rows of @p clause, if there is one;
    /// otherwise enter @p clause under @p id.
    /// @pre As for `find()`, and `id` is not entered.
    std::optional<ClauseId> find_or_enter(const Clause& clause, std::size_t key, ClauseId id,
                                          const std::vector<Clause>& clauses);

    /// @pre `id` is not entered.
    void enter(ClauseId id, std::size_t key);

    /// @pre `id` is entered.
    void remove(ClauseId id);

    bool is_entered(ClauseId id) const noexcept {
        return id < clause_id_to_key_.size() && clause_id_to_key_[id].has_value();
    }

    std::size_t size() const noexcept { return key_to_clause_ids_.size(); }

private:
    std::unordered_multimap<std::size_t, ClauseId> key_to_clause_ids_;
    std::vector<std::optional<std::size_t>> clause_id_to_key_;
};

} // namespace xorcle
