#include "heuristics/equivalent_clauses/equivalent_clause_table.h"

#include <cassert>
#include <utility>

namespace xorcle {

std::optional<ClauseId> EquivalentClauseTable::find(const Clause& clause, const std::size_t key,
                                                    const std::vector<Clause>& clauses) const {
    const auto [first, last] = key_to_clause_ids_.equal_range(key);
    for (auto entry = first; entry != last; ++entry) {
        if (clauses[entry->second].has_rows_of(clause)) {
            return entry->second;
        }
    }
    return std::nullopt;
}

std::optional<ClauseId> EquivalentClauseTable::find_or_enter(const Clause& clause,
                                                             const std::size_t key,
                                                             const ClauseId id,
                                                             const std::vector<Clause>& clauses) {
    assert(!is_entered(id) && "the clause is already entered");
    const std::optional<ClauseId> found_id = find(clause, key, clauses);
    if (!found_id.has_value()) {
        enter(id, key);
    }
    return found_id;
}

void EquivalentClauseTable::enter(const ClauseId id, const std::size_t key) {
    assert(!is_entered(id) && "the clause is already entered");
    if (id >= clause_id_to_key_.size()) {
        clause_id_to_key_.resize(id + 1);
    }
    clause_id_to_key_[id] = key;
    key_to_clause_ids_.emplace(key, id);
}

void EquivalentClauseTable::remove(const ClauseId id) {
    assert(is_entered(id) && "the clause is not entered");
    const auto [first, last] = key_to_clause_ids_.equal_range(*clause_id_to_key_[id]);
    for (auto entry = first; entry != last; ++entry) {
        if (entry->second == id) {
            key_to_clause_ids_.erase(entry);
            clause_id_to_key_[id].reset();
            return;
        }
    }
    assert(false && "an entered clause is missing under its key");
    std::unreachable();
}

} // namespace xorcle
