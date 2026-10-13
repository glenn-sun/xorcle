#pragma once

/// @file clause_selector.h
/// @brief Choosing which clause to propagate from next, and owning the clause database.

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "algebra/compaction_transform.h"
#include "algebra/equation.h"
#include "algebra/row_echelon_matrix.h"
#include "core/clause_id.h"
#include "core/trail.h"
#include "core/watch.h"
#include "util/index_set.h"
#include "util/stats.h"

namespace xorcle {

/// @brief A clause to try, and the trigger the selector already knows holds.
struct ClauseSelection {
    ClauseId id;
    std::optional<WatchType> trigger_hint;
};

enum class ClauseSelectorName {
    cyclic, ///< Round-robin over the clause database.
    exact_watchlist ///< Work queue of the clauses whose watch pair became dependent.
};

/// @brief What conflict analysis knows about a learned clause beyond its rows.
struct AssertingClauseInfo {
    std::size_t lbd;

    /// @brief The number of decision levels just *discarded*, which after a restart
    /// is neither the asserting level nor its complement.
    std::size_t removed_levels;

    /// @brief Whether this replaces a clause on-the-fly subsumption removed,
    /// rather than closing the conflict.
    bool is_replacement = false;
};

/// @brief Decides which clause the solver tries to propagate from next.
///
/// It is also the **authoritative clause store**: the solver keeps no clause vector of its
/// own, and a watchlist selector's indexes are a view of this one.
///
/// @pre Every accessor taking a clause id requires `id < num_clauses()`.
class ClauseSelector : public WatchUpdateHandler {
public:
    ClauseSelector() = default;

    ClauseSelector(const ClauseSelector&) = delete;
    ClauseSelector& operator=(const ClauseSelector&) = delete;

    ~ClauseSelector() override = default;

    std::size_t num_clauses() const noexcept { return clauses_.size(); }

    const std::vector<Clause>& clauses() const noexcept { return clauses_; }

    /// @brief The clause, whose rows only `replace_clause()` and a renumbering change.
    Clause& clause(ClauseId id) {
#ifndef NDEBUG
        debug_assert_clause_id(id);
#endif
        return clauses_[id];
    }

    void replace_clause(ClauseId id, Clause clause);

    /// @brief The active clauses that carry a column of @p col_idxs or span `0 = 1`,
    /// in ascending order; enters the clauses added since the last call.
    /// @pre `col_idxs` is sized like a clause's columns and leaves the last one out.
    std::vector<ClauseId> clause_ids_carrying(const IndexSet& col_idxs);

    bool is_clause_active(ClauseId id) const {
#ifndef NDEBUG
        debug_assert_clause_id(id);
#endif
        return clause_active_[id] != 0;
    }
    void set_clause_active(ClauseId id, bool active);

    const std::vector<std::uint8_t>& active_flags() const noexcept { return clause_active_; }

    ActiveClauseIds active_clause_ids() const noexcept {
        return {listed_clause_ids_, clause_active_};
    }

    /// @brief Whether the clause was learned rather than read from the instance.
    bool is_clause_learned(ClauseId id) const {
#ifndef NDEBUG
        debug_assert_clause_id(id);
#endif
        return clause_learned_[id] != 0;
    }

    /// @brief Mark a clause learned or not; what replaces an input clause takes
    /// its standing, so it is never a deletion candidate either.
    void set_clause_learned(ClauseId id, bool learned) {
#ifndef NDEBUG
        debug_assert_clause_id(id);
#endif
        clause_learned_[id] = learned ? 1 : 0;
    }

    /// @brief The LBD deletion reads, which a use as a reason moves toward the LBD measured there.
    /// @pre `is_clause_learned(id)`.
    double clause_lbd(ClauseId id) const {
#ifndef NDEBUG
        debug_assert_clause_id(id);
#endif
        assert(clause_learned_[id] != 0 && "only a learned clause has an LBD");
        return clause_lbd_[id];
    }
    /// @pre `is_clause_learned(id)`.
    void set_clause_lbd(ClauseId id, double lbd) {
#ifndef NDEBUG
        debug_assert_clause_id(id);
#endif
        assert(clause_learned_[id] != 0 && "only a learned clause has an LBD");
        clause_lbd_[id] = lbd;
    }

    bool is_clause_used_since_deletion(ClauseId id) const {
#ifndef NDEBUG
        debug_assert_clause_id(id);
#endif
        return clause_used_since_deletion_[id] != 0;
    }
    void set_clause_used_since_deletion(ClauseId id, bool used) {
#ifndef NDEBUG
        debug_assert_clause_id(id);
#endif
        clause_used_since_deletion_[id] = used ? 1 : 0;
    }

    /// @brief Consecutive deletion passes the clause has sat through unlocked and unused.
    std::size_t clause_unused_passes(ClauseId id) const {
#ifndef NDEBUG
        debug_assert_clause_id(id);
#endif
        return clause_unused_passes_[id];
    }
    void set_clause_unused_passes(ClauseId id, std::size_t num_passes) {
#ifndef NDEBUG
        debug_assert_clause_id(id);
#endif
        clause_unused_passes_[id] = num_passes;
    }

    /// @brief Whether the clause has gone unused for as many deletion passes as its LBD allowed.
    bool is_clause_out_of_unused_passes(ClauseId id) const {
#ifndef NDEBUG
        debug_assert_clause_id(id);
#endif
        return clause_out_of_unused_passes_[id] != 0;
    }
    void set_clause_out_of_unused_passes(ClauseId id, bool out) {
#ifndef NDEBUG
        debug_assert_clause_id(id);
#endif
        clause_out_of_unused_passes_[id] = out ? 1 : 0;
    }

    /// @brief The next clause to try, with the trigger when the selector knows it;
    /// `std::nullopt` once the propagation run is over.
    virtual std::optional<ClauseSelection> next() = 0;

    virtual void add_starting_clause(Clause clause); ///< Takes the next id.

    /// @brief Add a learned asserting clause under the next id; `trail` already reflects
    /// the backjump.
    virtual void add_asserting_clause(Clause clause, const Trail& trail,
                                      const AssertingClauseInfo& info);

    /// @brief Note an equation learned by propagation, already reduced by the span it was
    /// added to and already on `trail`.
    virtual void update_with_propagation(const Equation& reduced_eqn, const Trail& trail);

    virtual void update_with_decision(const Equation& reduced_eqn, const Trail& trail);

    /// @brief The default holds no reduction.
    HeldReduction copy_reduced_companion(ClauseId id, WatchType type, Equation& reduced,
                                         ReduceResult& result) override;

    /// @brief Note a watch move, which the default ignores.
    void update_with_new_watch(ClauseId id, ClauseRowIdx old_watch_row_idx, WatchType type,
                               const Trail& trail, const WatchReductions& reductions) override;

    /// @brief Note that the trail dropped every level above `level`, which the
    /// default ignores; `trail` already reflects it.
    virtual void update_with_backjump(std::size_t level, const Trail& trail);

    /// @brief Note that a substitution emptied the root trail.
    /// @param rewritten_clause_ids The clauses it replaced or deactivated; every
    /// other active clause carries no pivot of the trail it emptied.
    /// @param dropped_col_idxs The variables dropped since the last renumbering.
    virtual void update_with_substitution(std::span<const ClauseId> rewritten_clause_ids,
                                          const IndexSet& dropped_col_idxs, const Trail& trail);

    /// @brief Renumber every active clause, after `update_with_substitution()` told
    /// of the last variable dropped; no watch moves.
    /// @pre No dropped variable occurs in an active clause, and `trail` is empty.
    virtual void update_with_renumbering(const CompactionTransform& transform, const Trail& trail);

    /// @brief Add this selector's counters to `stats`.
    virtual void add_to_stats(RunStats& stats) const;

protected:
    /// @brief Store a clause under the next id, with no other side effects.
    virtual void add_clause(Clause clause);

    /// @brief React to a learned equation, propagated or decided alike.
    virtual void update_with_new_equation(const Equation& reduced_eqn, const Trail& trail);

    /// @brief Note that an active clause went inactive, which the default ignores.
    virtual void update_with_inactive_clause(ClauseId id);

    /// @brief Whether `col_idx_to_clause_ids_[col_idx]` is right to name the clause.
    bool carries(ClauseId id, std::size_t col_idx) const {
        return clause_active_[id] != 0 && clauses_[id].carries(col_idx);
    }

    bool spans_eq_false(ClauseId id) const {
        return clause_active_[id] != 0 && clause_spans_eq_false_[id] != 0;
    }

    /// @brief Enter an active clause under each column it carries and @p replaced,
    /// the clause its id held until now, does not; null for a new id.
    void enter_carried_col_idxs(ClauseId id, const Clause* replaced);

    /// @brief Drop the stale and repeated entries once they outnumber the rest.
    void maybe_clean_carrier_entries();

    /// @brief Test every entry against its clause; the number left.
    std::size_t drop_wrong_carrier_entries();

    /// @brief Clear every entry and enter each entered clause; the number of entries.
    std::size_t enter_every_clause_again();

#ifndef NDEBUG
    /// @brief What `clause_ids_carrying()` returns, read off every active clause.
    std::vector<ClauseId> debug_scan_clause_ids_carrying(const IndexSet& col_idxs) const;

    void debug_assert_database_synchronized() const;
    void debug_assert_clause_id(ClauseId id) const;
#endif


    std::vector<Clause> clauses_; ///< A clause's id is its index.
    std::vector<std::uint8_t> clause_spans_eq_false_;

    /// @brief Per variable column, the clauses that may carry it.
    ///
    /// `carries()` is the truth: an entry may be stale, and a clause may be entered twice.
    /// An active clause is entered under every column it carries from the first
    /// `clause_ids_carrying()` after it was added.
    std::vector<std::vector<ClauseId>> col_idx_to_clause_ids_;
    std::vector<ClauseId> clause_ids_spanning_eq_false_; ///< Entries likewise.
    std::size_t num_carrier_entries_ = 0; ///< Over both of the above.
    std::size_t peak_carrier_entries_ = 0;
    std::size_t carrier_cleanup_threshold_ = 0;
    std::vector<std::uint8_t> clause_entered_;
    std::vector<ClauseId> unentered_clause_ids_; ///< Added since the last `clause_ids_carrying()`.
    std::vector<std::uint8_t> clause_active_;
    /// @brief Ascending: every active id, and at most as many inactive ones.
    std::vector<ClauseId> listed_clause_ids_;
    std::size_t num_inactive_listed_ = 0; ///< Inactive ids still in `listed_clause_ids_`.
    std::vector<std::uint8_t> clause_learned_;
    std::vector<std::uint8_t> clause_used_since_deletion_;
    std::vector<double> clause_lbd_;
    std::vector<std::size_t> clause_unused_passes_;
    std::vector<std::uint8_t> clause_out_of_unused_passes_;
};

/// @brief Build the selector named by @p name over @p num_vars variables.
std::unique_ptr<ClauseSelector> make_clause_selector(ClauseSelectorName name,
                                                     std::size_t num_vars);

} // namespace xorcle
