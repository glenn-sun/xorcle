#include "heuristics/selection/cyclic.h"

#include <utility>

namespace xorcle {

Cyclic::Cyclic(std::size_t) {}

void Cyclic::stop(const bool by_budget) {
    count_ = 0;
    budget_count_ = 0;
    last_stop_was_budget_ = by_budget;
}

std::optional<ClauseSelection> Cyclic::next() {
    if (clauses_.empty()) {
        stop(false);
        return std::nullopt;
    }

    if (pending_decision_clause_.has_value()) {
        const ClauseId decision_clause_id = *pending_decision_clause_;
        pending_decision_clause_.reset();
        if (decision_clause_id < clauses_.size() && is_clause_active(decision_clause_id)) {
            id_ = decision_clause_id;
            count_ = 0;
        }
    }

    if (count_ >= clauses_.size()) {
        stop(false);
        return std::nullopt;
    }
    if (budget_ != 0 && budget_count_ >= budget_) {
        stop(true);
        return std::nullopt;
    }

    for (std::size_t attempts = 0; attempts < clauses_.size(); ++attempts) {
        const ClauseId current = id_;
        id_ = (id_ + 1) % clauses_.size();
        ++count_;
        ++budget_count_;
        if (is_clause_active(current)) {
            return ClauseSelection{current, std::nullopt};
        }
        if (count_ >= clauses_.size()) {
            stop(false);
            return std::nullopt;
        }
        if (budget_ != 0 && budget_count_ >= budget_) {
            stop(true);
            return std::nullopt;
        }
    }

    stop(false);
    return std::nullopt;
}

void Cyclic::set_attempt_budget(const std::size_t budget) {
    budget_ = budget;
    // The run this opens gets the whole budget however the last one ended: a
    // conflict leaves the count standing, since it never asks for a next clause.
    budget_count_ = 0;
}

bool Cyclic::last_stop_was_budget() const noexcept { return last_stop_was_budget_; }

void Cyclic::add_asserting_clause(Clause clause, const Trail& trail,
                                  const AssertingClauseInfo& info) {
    const ClauseId cursor = id_;
    const std::size_t count = count_;
    ClauseSelector::add_asserting_clause(std::move(clause), trail, info);
    if (info.is_replacement) {
        id_ = cursor;
        count_ = count;
    }
}

void Cyclic::add_clause(Clause clause) {
    ClauseSelector::add_clause(std::move(clause));
    id_ = clauses_.size() - 1;
    count_ = 0;
}

void Cyclic::update_with_new_equation(const Equation&, const Trail&) {
    // The cursor deliberately stays where it is: only the budgets are refreshed,
    // so propagation resumes from the clause after the one that just produced an
    // equation. The attempt budget bounds a fruitless stretch, so a stretch that
    // produced something starts it over.
    count_ = 0;
    budget_count_ = 0;
}

void Cyclic::update_with_decision_clause(std::optional<ClauseId> clause_id) {
    if (clause_id.has_value() && *clause_id < clauses_.size() && is_clause_active(*clause_id)) {
        pending_decision_clause_ = clause_id;
    } else {
        pending_decision_clause_.reset();
    }
}

void Cyclic::update_with_substitution(std::span<const ClauseId>, const IndexSet&,
                                      const Trail&) {
    if (clauses_.empty()) {
        id_ = 0;
        pending_decision_clause_.reset();
        stop(false);
        return;
    }

    if (id_ >= clauses_.size()) {
        id_ %= clauses_.size();
    }
    if (!is_clause_active(id_)) {
        // The next active clause from the cursor on, wrapping to the first.
        std::optional<ClauseId> first;
        for (const ClauseId candidate : active_clause_ids()) {
            if (!first.has_value()) {
                first = candidate;
            }
            if (candidate > id_) {
                first = candidate;
                break;
            }
        }
        id_ = first.value_or(id_);
    }
    if (pending_decision_clause_.has_value() && (*pending_decision_clause_ >= clauses_.size() ||
                                                !is_clause_active(*pending_decision_clause_))) {
        pending_decision_clause_.reset();
    }
    stop(false);
}

} // namespace xorcle
