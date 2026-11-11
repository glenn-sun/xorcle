#include "heuristics/selection/cyclic.h"

namespace xorcle {

Cyclic::Cyclic(const ClauseDatabase& database) : ClauseSelector(database) {}

void Cyclic::stop(const bool by_budget) {
    count_ = 0;
    budget_count_ = 0;
    last_stop_was_budget_ = by_budget;
}

std::optional<ClauseSelection> Cyclic::next() {
    if (database_.num_clauses() == 0) {
        stop(false);
        return std::nullopt;
    }

    if (pending_decision_clause_.has_value()) {
        id_ = *pending_decision_clause_;
        count_ = 0;
        pending_decision_clause_.reset();
    }

    if (count_ >= database_.num_clauses()) {
        stop(false);
        return std::nullopt;
    }
    if (budget_ != 0 && budget_count_ >= budget_) {
        stop(true);
        return std::nullopt;
    }

    for (std::size_t attempts = 0; attempts < database_.num_clauses(); ++attempts) {
        const ClauseId current = id_;
        id_ = (id_ + 1) % database_.num_clauses();
        ++count_;
        ++budget_count_;
        if (database_.is_clause_active(current)) {
            return ClauseSelection{current, std::nullopt};
        }
        if (count_ >= database_.num_clauses()) {
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

void Cyclic::update_with_asserting_clause(const ClauseId id, const Trail& trail,
                                          const AssertingClauseInfo& info) {
    const ClauseId cursor = id_;
    const std::size_t count = count_;
    ClauseSelector::update_with_asserting_clause(id, trail, info);
    if (info.is_replacement) {
        id_ = cursor;
        count_ = count;
    }
}

void Cyclic::update_with_new_clause(const ClauseId id) {
    id_ = id;
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

void Cyclic::update_with_decision(const Equation& reduced_eqn, const Trail& trail,
                                  std::optional<ClauseId> decision_clause) {
    update_with_new_equation(reduced_eqn, trail);
    pending_decision_clause_ = decision_clause;
}

void Cyclic::update_with_substitution(std::span<const ClauseId>, const IndexSet&,
                                      const Trail&) {
    if (database_.num_clauses() == 0) {
        id_ = 0;
        pending_decision_clause_.reset();
        stop(false);
        return;
    }

    if (id_ >= database_.num_clauses()) {
        id_ %= database_.num_clauses();
    }
    if (!database_.is_clause_active(id_)) {
        // The next active clause from the cursor on, wrapping to the first.
        std::optional<ClauseId> first;
        for (const ClauseId candidate : database_.active_clause_ids()) {
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
    if (pending_decision_clause_.has_value() &&
        (*pending_decision_clause_ >= database_.num_clauses() ||
         !database_.is_clause_active(*pending_decision_clause_))) {
        pending_decision_clause_.reset();
    }
    stop(false);
}

} // namespace xorcle
