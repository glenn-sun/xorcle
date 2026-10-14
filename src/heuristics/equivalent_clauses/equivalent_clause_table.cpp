#include "heuristics/equivalent_clauses/equivalent_clause_table.h"

#include <algorithm>
#include <bit>
#include <cassert>

namespace xorcle {

std::optional<ClauseId> EquivalentClauseTable::find(const Clause& clause, const std::size_t key,
                                                    const std::vector<Clause>& clauses) const {
    if (slots_.empty()) {
        return std::nullopt;
    }
    const std::size_t mask = slots_.size() - 1;
    for (std::size_t slot_idx = first_slot_idx(key);; slot_idx = (slot_idx + 1) & mask) {
        const Slot& slot = slots_[slot_idx];
        if (slot.id == empty_slot) {
            return std::nullopt;
        }
        if (slot.id != removed_slot && slot.key == key && clauses[slot.id].has_rows_of(clause)) {
            return slot.id;
        }
    }
}

std::optional<ClauseId> EquivalentClauseTable::find_or_enter(const Clause& clause,
                                                             const std::size_t key,
                                                             const ClauseId id,
                                                             const std::vector<Clause>& clauses) {
    assert(!is_entered(id) && "the clause is already entered");
    reserve_for(id);
    const std::size_t mask = slots_.size() - 1;
    std::optional<std::size_t> first_removed_slot_idx;
    std::size_t slot_idx = first_slot_idx(key);
    for (;; slot_idx = (slot_idx + 1) & mask) {
        const Slot& slot = slots_[slot_idx];
        if (slot.id == empty_slot) {
            break;
        }
        if (slot.id == removed_slot) {
            if (!first_removed_slot_idx.has_value()) {
                first_removed_slot_idx = slot_idx;
            }
        } else if (slot.key == key && clauses[slot.id].has_rows_of(clause)) {
            return slot.id;
        }
    }
    fill(first_removed_slot_idx.value_or(slot_idx), key, id);
    return std::nullopt;
}

void EquivalentClauseTable::enter(const ClauseId id, const std::size_t key) {
    assert(!is_entered(id) && "the clause is already entered");
    reserve_for(id);
    const std::size_t mask = slots_.size() - 1;
    std::size_t slot_idx = first_slot_idx(key);
    while (slots_[slot_idx].id != empty_slot && slots_[slot_idx].id != removed_slot) {
        slot_idx = (slot_idx + 1) & mask;
    }
    fill(slot_idx, key, id);
}

void EquivalentClauseTable::reserve_for(const ClauseId id) {
    // At most half the slots are used, so a probe ends.
    if (2 * (num_used_slots_ + 1) > slots_.size()) {
        rehash();
    }
    if (id >= clause_id_to_slot_idx_.size()) {
        clause_id_to_slot_idx_.resize(
            std::max<std::size_t>(id + 1, 2 * clause_id_to_slot_idx_.size()), not_entered);
    }
}

void EquivalentClauseTable::fill(const std::size_t slot_idx, const std::size_t key,
                                 const ClauseId id) {
    num_used_slots_ += slots_[slot_idx].id == empty_slot ? 1U : 0U;
    slots_[slot_idx] = Slot{key, id};
    clause_id_to_slot_idx_[id] = static_cast<std::uint32_t>(slot_idx);
    ++num_entered_;
}

void EquivalentClauseTable::remove(const ClauseId id) {
    if (!is_entered(id)) {
        return;
    }
    slots_[clause_id_to_slot_idx_[id]].id = removed_slot;
    clause_id_to_slot_idx_[id] = not_entered;
    --num_entered_;
}

void EquivalentClauseTable::rehash() {
    const std::size_t num_slots = std::bit_ceil(std::max<std::size_t>(16, 4 * (num_entered_ + 1)));
    assert(num_slots <= not_entered && "more slots than a slot index holds");
    std::vector<Slot> old_slots(num_slots, Slot{0, empty_slot});
    old_slots.swap(slots_);
    shift_ = static_cast<unsigned>(64 - std::countr_zero(num_slots));
    const std::size_t mask = num_slots - 1;
    for (const Slot& slot : old_slots) {
        if (slot.id == empty_slot || slot.id == removed_slot) {
            continue;
        }
        std::size_t slot_idx = first_slot_idx(slot.key);
        while (slots_[slot_idx].id != empty_slot) {
            slot_idx = (slot_idx + 1) & mask;
        }
        slots_[slot_idx] = slot;
        clause_id_to_slot_idx_[slot.id] = static_cast<std::uint32_t>(slot_idx);
    }
    num_used_slots_ = num_entered_;
}

} // namespace xorcle
