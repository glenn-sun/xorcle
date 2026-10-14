#pragma once

/// @file equivalent_clause_table.h
/// @brief Finding the kept clause whose rows are those of a given clause.

#include <cstddef>
#include <cstdint>
#include <optional>
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

    /// @brief Remove the clause's entry, if it has one.
    void remove(ClauseId id);

    bool is_entered(ClauseId id) const noexcept {
        return id < clause_id_to_slot_idx_.size() && clause_id_to_slot_idx_[id] != not_entered;
    }

    std::size_t size() const noexcept { return num_entered_; }

private:
    struct Slot {
        std::size_t key;
        ClauseId id;
    };

    static constexpr ClauseId empty_slot = static_cast<ClauseId>(-1);
    static constexpr ClauseId removed_slot = static_cast<ClauseId>(-2);
    static constexpr std::uint32_t not_entered = static_cast<std::uint32_t>(-1);

    /// @brief Where the probe for @p key starts.
    std::size_t first_slot_idx(std::size_t key) const noexcept { return key >> shift_; }

    /// @brief Move the entries to a table four times their number, without the removed slots.
    void rehash();

    /// @brief Make room for one more entry and for `id` in the slot index.
    void reserve_for(ClauseId id);

    void fill(std::size_t slot_idx, std::size_t key, ClauseId id);

    /// @brief Open addressing with linear probing over a power of two of slots.
    std::vector<Slot> slots_;
    unsigned shift_ = 0;
    std::vector<std::uint32_t> clause_id_to_slot_idx_;
    std::size_t num_entered_ = 0;
    std::size_t num_used_slots_ = 0; ///< Entered or removed.
};

} // namespace xorcle
