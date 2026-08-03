#include "heuristics/selection/cyclic.h"

#include <utility>

namespace xorcle {

Cyclic::Cyclic(std::size_t) {}

std::optional<ClauseId> Cyclic::next() {
    if (clauses_.empty()) {
        count_ = 0;
        return std::nullopt;
    }

    if (pending_restart_clause_.has_value()) {
        const ClauseId restart_id = *pending_restart_clause_;
        pending_restart_clause_.reset();
        if (restart_id < clauses_.size() && is_clause_active(restart_id)) {
            id_ = restart_id;
            count_ = 0;
        }
    }

    if (count_ >= clauses_.size()) {
        count_ = 0;
        return std::nullopt;
    }

    for (std::size_t attempts = 0; attempts < clauses_.size(); ++attempts) {
        const ClauseId current = id_;
        id_ = (id_ + 1) % clauses_.size();
        ++count_;
        if (is_clause_active(current)) {
            return current;
        }
        if (count_ >= clauses_.size()) {
            count_ = 0;
            return std::nullopt;
        }
    }

    count_ = 0;
    return std::nullopt;
}

void Cyclic::add_clause(Clause clause) {
    ClauseSelector::add_clause(std::move(clause));
    id_ = clauses_.size() - 1;
    count_ = 0;
}

void Cyclic::update_with_new_equation(const Equation&, const Trail&) {
    // The cursor deliberately stays where it is: only the quiescence budget is
    // refreshed, so propagation resumes from the clause after the one that just
    // produced an equation.
    count_ = 0;
}

void Cyclic::update_with_decision_clause(std::optional<ClauseId> clause_id) {
    if (clause_id.has_value() && *clause_id < clauses_.size() && is_clause_active(*clause_id)) {
        pending_restart_clause_ = clause_id;
    } else {
        pending_restart_clause_.reset();
    }
}

void Cyclic::update_with_compaction(const CompactionTransform&, const Trail&) {
    if (clauses_.empty()) {
        id_ = 0;
        count_ = 0;
        pending_restart_clause_.reset();
        return;
    }

    if (id_ >= clauses_.size()) {
        id_ %= clauses_.size();
    }
    if (!is_clause_active(id_)) {
        for (std::size_t offset = 0; offset < clauses_.size(); ++offset) {
            const ClauseId candidate = (id_ + offset) % clauses_.size();
            if (is_clause_active(candidate)) {
                id_ = candidate;
                break;
            }
        }
    }
    if (pending_restart_clause_.has_value() && (*pending_restart_clause_ >= clauses_.size() ||
                                                !is_clause_active(*pending_restart_clause_))) {
        pending_restart_clause_.reset();
    }
    count_ = 0;
}

} // namespace xorcle
