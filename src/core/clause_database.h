#pragma once

/// @file clause_database.h
/// @brief The clauses and what the solver keeps of each.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "algebra/compaction_transform.h"
#include "algebra/row_echelon_matrix.h"
#include "core/clause_id.h"
#include "core/watch.h"
#include "util/index_set.h"

namespace xorcle {

/// @brief What the solver keeps of a clause beside its rows.
struct ClauseRecord {
    ClauseWatches watches;

    /// @brief The decision level whose span was found to satisfy this clause.
    ///
    /// Spans nest, so a clause satisfied at a level stays satisfied at every
    /// deeper one.
    std::size_t satisfied_level = 0;
    /// @brief The stamp `satisfied_level` carried then, which tells it from the
    /// level of the same depth a later branch builds; `0` is no record.
    std::size_t satisfied_stamp = 0;

    /// @brief The average of the LBDs measured at the clause's uses as a reason, a learned
    /// clause's starting at the LBD it was learned at; read only when `lbd_measured`.
    double lbd = 0.0;

    /// @brief Consecutive deletion passes the clause has sat through unlocked and unused.
    std::size_t unused_passes = 0;

    /// @brief Whether the clause was learned rather than read from the instance; what
    /// replaces an input clause takes its standing, so it is never a deletion candidate either.
    bool learned = false;

    bool used_since_deletion = false;

    /// @brief Whether a vivification pass has been through the clause since it was
    /// added or last changed width.
    bool vivified = false;

    /// @brief Whether an inprocessing subsumption pass has been through the clause
    /// since it was added or last changed.
    bool inproc_subsume_strengthen_checked = false;

    /// @brief Whether `lbd` holds a measurement, which a starting clause lacks until its
    /// first use as a reason.
    bool lbd_measured = false;

    /// @brief Take the LBD measured at a use as a reason: the first measurement becomes
    /// `lbd`, and each later one is averaged in with weight `1 / lbd_window`.
    /// @pre `lbd_window >= 1`.
    void add_lbd_measurement(std::size_t measured_lbd, std::size_t lbd_window);

    /// @brief Keep the lower of the clause's measured LBD and @p other_lbd, an equivalent
    /// clause's, unset when unmeasured.
    void take_lower_lbd(std::optional<double> other_lbd);
};

/// @brief Every clause the solver holds, under ids that are never reused, with
/// its record and whether it is active.
///
/// A clause enters watching its first rows, with no record of what satisfied it.
///
/// @pre Every accessor taking a clause id requires `id < num_clauses()`.
class ClauseDatabase {
public:
    ClauseDatabase() = default;

    ClauseDatabase(const ClauseDatabase&) = delete;
    ClauseDatabase& operator=(const ClauseDatabase&) = delete;

    std::size_t num_clauses() const noexcept { return clauses_.size(); }

    const std::vector<Clause>& clauses() const noexcept { return clauses_; }

    /// @brief The clause, whose rows only `replace_clause()` and `renumber()` change.
    const Clause& clause(ClauseId id) const {
#ifndef NDEBUG
        debug_assert_clause_id(id);
#endif
        return clauses_[id];
    }

    ClauseRecord& record(ClauseId id) {
#ifndef NDEBUG
        debug_assert_clause_id(id);
#endif
        return records_[id];
    }
    const ClauseRecord& record(ClauseId id) const {
#ifndef NDEBUG
        debug_assert_clause_id(id);
#endif
        return records_[id];
    }

    std::span<const ClauseRecord> records() const noexcept { return records_; }

    bool is_clause_active(ClauseId id) const {
#ifndef NDEBUG
        debug_assert_clause_id(id);
#endif
        return clause_active_[id] != 0;
    }

    const std::vector<std::uint8_t>& active_flags() const noexcept { return clause_active_; }

    ActiveClauseIds active_clause_ids() const noexcept {
        return {listed_clause_ids_, clause_active_};
    }

    /// @brief The number of active clauses of each width, short of the widths none has had.
    std::span<const std::size_t> num_active_clauses_by_width() const noexcept {
        return num_active_clauses_by_width_;
    }

    /// @brief Clauses that entered the database at width 1 or were narrowed to it,
    /// over the whole solve.
    std::size_t num_clauses_reaching_width_1() const noexcept {
        return num_clauses_reaching_width_1_;
    }

    /// @brief As `num_clauses_reaching_width_1()`, of width 2.
    std::size_t num_clauses_reaching_width_2() const noexcept {
        return num_clauses_reaching_width_2_;
    }

#ifndef NDEBUG
    /// @brief Hold `num_active_clauses_by_width()` to a count over every active clause.
    void debug_assert_widths_counted() const;
#endif

    /// @brief Add a clause from the instance, or one standing for such clauses, under
    /// the next id, which it returns.
    ClauseId add_starting_clause(Clause clause);

    /// @brief Add a learned clause of LBD @p lbd under the next id, which it returns.
    ClauseId add_learned_clause(Clause clause, std::size_t lbd);

    /// @brief Change the clause at @p id in place; a width-1 or width-2 clause counts
    /// in `num_clauses_reaching_width_1()` or `num_clauses_reaching_width_2()` only if
    /// it was another width before.
    void replace_clause(ClauseId id, Clause clause);

    /// @brief Put a new clause at the id of an active clause it takes the place of,
    /// counting it as `add_starting_clause()` would.
    /// @pre The clause at @p id is active.
    void replace_with_new_clause(ClauseId id, Clause clause);

    /// @pre The clause is active.
    void deactivate_clause(ClauseId id);

    /// @brief Renumber every active clause from the old numbering to the compacted one.
    /// @pre No dropped variable occurs in an active clause.
    void renumber(const CompactionTransform& transform);

    /// @brief The active clauses that carry a column of @p col_idxs or span `0 = 1`,
    /// in ascending order; enters the clauses added since the last call.
    /// @pre `col_idxs` is sized like a clause's columns and leaves the last one out.
    std::vector<ClauseId> clause_ids_carrying(const IndexSet& col_idxs);

private:
    /// @brief Store a clause under the next id.
    ClauseId add_clause(Clause clause);

    /// @brief Count an active clause of @p width in or out of `num_active_clauses_by_width_`.
    void count_width(std::size_t width, bool in) {
        if (num_active_clauses_by_width_.size() <= width) {
            num_active_clauses_by_width_.resize(width + 1, 0);
        }
        if (in) {
            ++num_active_clauses_by_width_[width];
        } else {
            --num_active_clauses_by_width_[width];
        }
    }

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
    std::vector<ClauseRecord> records_;
    std::vector<std::uint8_t> clause_active_;
    /// @brief Ascending: every active id, and at most as many inactive ones.
    std::vector<ClauseId> listed_clause_ids_;
    std::size_t num_inactive_listed_ = 0; ///< Inactive ids still in `listed_clause_ids_`.
    std::vector<std::size_t> num_active_clauses_by_width_;
    std::size_t num_clauses_reaching_width_1_ = 0;
    std::size_t num_clauses_reaching_width_2_ = 0;

    std::vector<std::uint8_t> clause_spans_eq_false_;
    /// @brief Per variable column, the clauses that may carry it.
    ///
    /// `carries()` is the truth: an entry may be stale, and a clause may be entered twice.
    /// An active clause is entered under every column it carries from the first
    /// `clause_ids_carrying()` after it was added.
    std::vector<std::vector<ClauseId>> col_idx_to_clause_ids_;
    std::vector<ClauseId> clause_ids_spanning_eq_false_; ///< Entries likewise.
    std::size_t num_carrier_entries_ = 0; ///< Over both of the above.
    std::size_t carrier_cleanup_threshold_ = 0;
    std::vector<std::uint8_t> clause_entered_;
    std::vector<ClauseId> unentered_clause_ids_; ///< Added since the last `clause_ids_carrying()`.
};

} // namespace xorcle
