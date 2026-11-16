#include "core/clause_database.h"

#include <cassert>
#include <optional>
#include <utility>

#include "algebra/dense/dense_gf2_vector.h"

namespace xorcle {

void ClauseRecord::add_lbd_measurement(const std::size_t measured_lbd,
                                       const std::size_t lbd_window) {
    assert(lbd_window >= 1 && "the LBD window is empty");
    if (!lbd_measured) {
        lbd = static_cast<double>(measured_lbd);
        lbd_measured = true;
        return;
    }
    const double weight = 1.0 / static_cast<double>(lbd_window);
    lbd = (1.0 - weight) * lbd + weight * static_cast<double>(measured_lbd);
}

void ClauseRecord::take_lower_lbd(const std::optional<double> other_lbd) {
    if (other_lbd.has_value() && (!lbd_measured || *other_lbd < lbd)) {
        lbd = *other_lbd;
        lbd_measured = true;
    }
}

#ifndef NDEBUG
void ClauseDatabase::debug_assert_database_synchronized() const {
    assert(clauses_.size() == clause_active_.size() &&
           "clause database and activity flags are desynchronized");
    assert(clauses_.size() == records_.size() &&
           clauses_.size() == clause_spans_eq_false_.size() &&
           clauses_.size() == clause_entered_.size() &&
           "clause database and metadata are desynchronized");
}

void ClauseDatabase::debug_assert_widths_counted() const {
    std::vector<std::size_t> counted(num_active_clauses_by_width_.size(), 0);
    for (const ClauseId id : active_clause_ids()) {
        assert(clauses_[id].rows() < counted.size() &&
               "an active clause's width was never counted");
        ++counted[clauses_[id].rows()];
    }
    assert(counted == num_active_clauses_by_width_ &&
           "the active clauses by width are miscounted");
}

void ClauseDatabase::debug_assert_clause_id(ClauseId id) const {
    debug_assert_database_synchronized();
    assert(id < clauses_.size() && "clause id is out of range");
}
#endif

void ClauseDatabase::replace_clause(ClauseId id, Clause clause) {
#ifndef NDEBUG
    debug_assert_clause_id(id);
#endif
    const Clause replaced = std::exchange(clauses_[id], std::move(clause));
    ClauseRecord& record = records_[id];
    record.watches = ClauseWatches::first_rows(clauses_[id].rows());
    record.satisfied_level = 0;
    record.satisfied_stamp = 0;
    if (replaced.rows() != clauses_[id].rows()) {
        record.vivified = false;
    }
    record.inproc_subsume_strengthen_checked = false;
    if (clause_active_[id] != 0) {
        count_width(replaced.rows(), false);
        count_width(clauses_[id].rows(), true);
        num_clauses_reaching_width_1_ += replaced.rows() != 1 && clauses_[id].rows() == 1 ? 1U : 0U;
        num_clauses_reaching_width_2_ += replaced.rows() != 2 && clauses_[id].rows() == 2 ? 1U : 0U;
    }
    if (clause_active_[id] != 0 && clause_entered_[id] != 0) {
        assert(replaced.cols() == clauses_[id].cols() &&
               "an active clause was replaced by one over another numbering");
        enter_carried_col_idxs(id, &replaced);
        maybe_clean_carrier_entries();
    }
}

void ClauseDatabase::replace_with_new_clause(const ClauseId id, Clause clause) {
    assert(is_clause_active(id) && "a new clause took the id of an inactive one");
    const bool width_1_uncounted = clauses_[id].rows() == 1 && clause.rows() == 1;
    const bool width_2_uncounted = clauses_[id].rows() == 2 && clause.rows() == 2;
    replace_clause(id, std::move(clause));
    num_clauses_reaching_width_1_ += width_1_uncounted ? 1U : 0U;
    num_clauses_reaching_width_2_ += width_2_uncounted ? 1U : 0U;
}

void ClauseDatabase::enter_carried_col_idxs(const ClauseId id, const Clause* const replaced) {
    const IndexSet carried = clauses_[id].carried_col_idxs();
    const std::size_t constant_col_idx = carried.size() - 1;
    if (col_idx_to_clause_ids_.size() < constant_col_idx) {
        col_idx_to_clause_ids_.resize(constant_col_idx);
    }
    // Testing a column against the replaced clause's rows beats taking their union
    // until as many columns are tested as a row has words.
    std::size_t num_row_tests_left =
        (carried.size() + DenseGF2Vector::word_mask) >> DenseGF2Vector::word_shift;
    IndexSet replaced_carried;
    const auto replaced_carries = [&](const std::size_t col_idx) {
        if (num_row_tests_left == 0) {
            if (replaced_carried.size() == 0) {
                replaced_carried = replaced->carried_col_idxs();
            }
            return replaced_carried.test(col_idx);
        }
        --num_row_tests_left;
        return replaced->carries(col_idx);
    };
    carried.for_each_set([&](const std::size_t col_idx) {
        if (col_idx != constant_col_idx && !(replaced != nullptr && replaced_carries(col_idx))) {
            col_idx_to_clause_ids_[col_idx].push_back(id);
            ++num_carrier_entries_;
        }
    });
    clause_spans_eq_false_[id] = clauses_[id].pivot_col_idxs().test(constant_col_idx) ? 1 : 0;
    if (spans_eq_false(id)) {
        clause_ids_spanning_eq_false_.push_back(id);
        ++num_carrier_entries_;
    }
}

void ClauseDatabase::maybe_clean_carrier_entries() {
    if (num_carrier_entries_ <= carrier_cleanup_threshold_) {
        return;
    }
    // Testing an entry reads one word of each row and entering a clause reads
    // every word of each, so entering every clause again pays once the entries
    // outnumber the words of the active clauses' rows.
    const std::size_t num_words = DenseGF2Vector::word_count(col_idx_to_clause_ids_.size() + 1);
    const std::size_t num_active_clauses = listed_clause_ids_.size() - num_inactive_listed_;
    const std::size_t kept = num_carrier_entries_ > num_active_clauses * num_words
                                 ? enter_every_clause_again()
                                 : drop_wrong_carrier_entries();
    num_carrier_entries_ = kept;
    carrier_cleanup_threshold_ = 2 * kept + col_idx_to_clause_ids_.size();
}

std::size_t ClauseDatabase::drop_wrong_carrier_entries() {
    IndexSet kept_ids(clauses_.size()); // over clause ids; empty between lists
    const auto clean = [&kept_ids](std::vector<ClauseId>& ids, const auto& is_right) {
        std::erase_if(ids, [&](ClauseId id) {
            if (kept_ids.test(id) || !is_right(id)) {
                return true;
            }
            kept_ids.set(id);
            return false;
        });
        for (const ClauseId id : ids) {
            kept_ids.reset(id);
        }
        return ids.size();
    };
    std::size_t kept = clean(clause_ids_spanning_eq_false_,
                             [this](ClauseId id) { return spans_eq_false(id); });
    for (std::size_t col_idx = 0; col_idx < col_idx_to_clause_ids_.size(); ++col_idx) {
        kept += clean(col_idx_to_clause_ids_[col_idx],
                      [this, col_idx](ClauseId id) { return carries(id, col_idx); });
    }
    return kept;
}

std::size_t ClauseDatabase::enter_every_clause_again() {
    // Every right entry is of an entered active clause and a column it carries.
    for (std::vector<ClauseId>& ids : col_idx_to_clause_ids_) {
        ids.clear();
    }
    clause_ids_spanning_eq_false_.clear();
    std::size_t kept = 0;
    for (const ClauseId id : active_clause_ids()) {
        if (clause_entered_[id] == 0) {
            continue;
        }
        const IndexSet carried = clauses_[id].carried_col_idxs();
        const std::size_t constant_col_idx = carried.size() - 1;
        carried.for_each_set([&](const std::size_t col_idx) {
            if (col_idx != constant_col_idx) {
                col_idx_to_clause_ids_[col_idx].push_back(id);
                ++kept;
            }
        });
        if (clause_spans_eq_false_[id] != 0) {
            clause_ids_spanning_eq_false_.push_back(id);
            ++kept;
        }
    }
    return kept;
}

std::vector<ClauseId> ClauseDatabase::clause_ids_carrying(const IndexSet& col_idxs) {
    for (const ClauseId id : unentered_clause_ids_) {
        if (clause_active_[id] != 0) {
            clause_entered_[id] = 1;
            enter_carried_col_idxs(id, nullptr);
        }
    }
    unentered_clause_ids_.clear();
    maybe_clean_carrier_entries();

    IndexSet found(clauses_.size()); // over clause ids
    col_idxs.for_each_set([&](const std::size_t col_idx) {
        if (col_idx >= col_idx_to_clause_ids_.size()) {
            return;
        }
        for (const ClauseId id : col_idx_to_clause_ids_[col_idx]) {
            if (!found.test(id) && carries(id, col_idx)) {
                found.set(id);
            }
        }
    });
    for (const ClauseId id : clause_ids_spanning_eq_false_) {
        if (spans_eq_false(id)) {
            found.set(id);
        }
    }
    std::vector<ClauseId> ids;
    found.for_each_set([&ids](const std::size_t id) { ids.push_back(id); });
    assert(ids == debug_scan_clause_ids_carrying(col_idxs) &&
           "the column-to-clause entries miss a clause or name a wrong one");
    return ids;
}

#ifndef NDEBUG
std::vector<ClauseId> ClauseDatabase::debug_scan_clause_ids_carrying(
    const IndexSet& col_idxs) const {
    std::vector<ClauseId> ids;
    for (const ClauseId id : active_clause_ids()) {
        const Clause& clause = clauses_[id];
        if (clause.pivot_col_idxs().test(clause.cols() - 1) ||
            clause.carried_col_idxs().intersects(col_idxs)) {
            ids.push_back(id);
        }
    }
    return ids;
}
#endif

void ClauseDatabase::deactivate_clause(ClauseId id) {
#ifndef NDEBUG
    debug_assert_clause_id(id);
#endif
    assert(clause_active_[id] != 0 && "an inactive clause was deactivated");
    clause_active_[id] = 0;
    count_width(clauses_[id].rows(), false);
    if (2 * ++num_inactive_listed_ > listed_clause_ids_.size()) {
        std::erase_if(listed_clause_ids_,
                      [this](ClauseId listed) { return clause_active_[listed] == 0; });
        num_inactive_listed_ = 0;
    }
}

void ClauseDatabase::renumber(const CompactionTransform& transform) {
    for (const ClauseId id : active_clause_ids()) {
#ifndef NDEBUG
        clauses_[id].carried_col_idxs().for_each_set([&](const std::size_t col_idx) {
            assert((col_idx == transform.old_num_vars() ||
                    transform.to_new(col_idx) != CompactionTransform::npos) &&
                   "a clause carries a dropped column at a renumbering");
        });
#endif
        clauses_[id].compact(transform);
    }
    // A dropped column's entries are stale.
    transform.move_by_column(col_idx_to_clause_ids_, [this](const std::vector<ClauseId>& ids) {
        num_carrier_entries_ -= ids.size();
    });
}

ClauseId ClauseDatabase::add_starting_clause(Clause clause) {
    return add_clause(std::move(clause));
}

ClauseId ClauseDatabase::add_learned_clause(Clause clause, const std::size_t lbd) {
    const ClauseId id = add_clause(std::move(clause));
    records_[id].learned = true;
    records_[id].lbd = static_cast<double>(lbd);
    records_[id].lbd_measured = true;
    return id;
}

ClauseId ClauseDatabase::add_clause(Clause clause) {
    listed_clause_ids_.push_back(clauses_.size());
    const ClauseId id = clauses_.size();
    clause_spans_eq_false_.push_back(0);
    count_width(clause.rows(), true);
    num_clauses_reaching_width_1_ += clause.rows() == 1 ? 1U : 0U;
    num_clauses_reaching_width_2_ += clause.rows() == 2 ? 1U : 0U;
    records_.push_back(ClauseRecord{.watches = ClauseWatches::first_rows(clause.rows())});
    clauses_.push_back(std::move(clause));
    clause_active_.push_back(1);
    clause_entered_.push_back(0);
    unentered_clause_ids_.push_back(id);
    return id;
}

} // namespace xorcle
