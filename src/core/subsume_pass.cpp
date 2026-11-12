/// @file subsume_pass.cpp
/// @brief The subsumption pass: deleting the clauses another subsumes and taking a
/// row off the clauses another strengthens.

#include "core/subsume_pass.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "algebra/equation.h"
#include "algebra/row_echelon_matrix.h"
#include "core/solver.h"
#include "util/index_set.h"
#include "util/random.h"

namespace xorcle {

PairCheck::PairCheck(const EquationRepr repr, const std::size_t num_vars)
    : repr_(repr), num_vars_(num_vars), reduced_(repr, num_vars) {}

PairRelation PairCheck::relate(const Clause& narrower, const Clause& wider) {
    assert(narrower.num_vars() == num_vars_ && wider.num_vars() == num_vars_ &&
           "a pair check is over another numbering than the clauses");
    assert(narrower.rows() != 0 && !narrower.pivot_col_idxs().test(num_vars_) &&
           !wider.pivot_col_idxs().test(num_vars_) &&
           "a pair check is on the empty clause or on a clause spanning 0 = 1");
    if (coefficients_.size() < narrower.rows()) {
        coefficients_.resize(narrower.rows());
        reduces_to_eq_false_.resize(narrower.rows());
    }
    bool strengthens = false;
    for (std::size_t row_idx = 0; row_idx < narrower.rows(); ++row_idx) {
        reduced_.assign_from(narrower.row(row_idx));
        wider.reduce_get_coefficients(reduced_, coefficients_[row_idx]);
        num_rows_read_ += wider.rows();
        if (reduced_.is_eq_true()) {
            reduces_to_eq_false_[row_idx] = 0;
        } else if (reduced_.is_eq_false()) {
            reduces_to_eq_false_[row_idx] = 1;
            strengthens = true;
        } else {
            return PairRelation::none;
        }
    }
    num_narrower_rows_ = narrower.rows();
    return strengthens ? PairRelation::strengthens : PairRelation::subsumes;
}

Clause PairCheck::strengthened(const Clause& wider, Rng& rng) const {
    // A hyperplane of W_D implied by C and D is the kernel of a functional on W_D
    // that is 1 exactly on the parts in W_D of C's rows that reduced to 0 = 1.
    // Over the coordinates of D's rows these are the equations `f . a = b` for each
    // row of C, which reduced by the rows `a` to `0 = b`; the free coordinates of
    // that system are drawn uniformly, so every solution is equally likely.
    const std::size_t width = wider.rows();
    RowSpace system(repr_, width);
    std::vector<std::size_t> bits;
    for (std::size_t row_idx = 0; row_idx < num_narrower_rows_; ++row_idx) {
        bits.clear();
        coefficients_[row_idx].for_each_set([&bits](const std::size_t i) { bits.push_back(i); });
        if (reduces_to_eq_false_[row_idx] != 0) {
            bits.push_back(width);
        }
        Equation constraint(repr_, width);
        constraint.assign_bits(bits);
        [[maybe_unused]] const std::optional<std::size_t> added =
            system.add_row_get_row_idx(std::move(constraint));
        assert(added.has_value() && "the narrower clause's rows are not independent over W_D");
    }
    for (std::size_t coordinate = 0; coordinate < width; ++coordinate) {
        if (system.pivot_col_idxs().test(coordinate)) {
            continue;
        }
        bits.assign(1, coordinate);
        if ((rng() >> 63) != 0) {
            bits.push_back(width);
        }
        Equation draw(repr_, width);
        draw.assign_bits(bits);
        system.add_row(std::move(draw));
    }
    const IndexSet functional = system.unique_solution();

    std::optional<std::size_t> kept_out;
    functional.for_each_set([&kept_out](const std::size_t i) {
        if (!kept_out.has_value()) {
            kept_out = i;
        }
    });
    assert(kept_out.has_value() && "a strengthening's functional is zero");
    Clause hyperplane(repr_, num_vars_);
    for (std::size_t row_idx = 0; row_idx < width; ++row_idx) {
        if (row_idx == *kept_out) {
            continue;
        }
        Equation row = wider.row(row_idx);
        if (functional.test(row_idx)) {
            row.plus_assign(wider.row(*kept_out));
        }
        hyperplane.add_row(std::move(row));
    }
    hyperplane.reduce_rows();
    assert(hyperplane.rows() + 1 == width && "a strengthening did not take exactly one row off");
    return hyperplane;
}

bool Solver::subsume_pass_is_due() const {
    if (!subsume_pass_.has_value()) {
        return false;
    }
    const std::size_t conflicts_since = stats_.num_conflicts - conflicts_at_last_subsume_pass_;
    return (!subsume_pass_has_run_ || conflicts_since != 0) &&
           conflicts_since >= subsume_pass_->cadence;
}

void Solver::run_subsume_pass() {
    assert(trail_.level() == 0 && "a subsumption pass runs at the root");
    phase_timer_.switch_to(SolverPhase::subsume_pass);
    conflicts_at_last_subsume_pass_ = stats_.num_conflicts;
    subsume_pass_has_run_ = true;
    const std::size_t rows = search_rows();
    // Rows read in the checks.
    const double budget =
        subsume_pass_->effort * static_cast<double>(rows - search_rows_at_last_subsume_pass_);
    search_rows_at_last_subsume_pass_ = rows;

    const std::size_t constant_col_idx = active_num_vars_;
    const IndexSet is_locked = trail_.reason_clause_ids(clause_database_.num_clauses());
    // Every indexed clause is filed under each of its leading columns, where a
    // visited narrower clause looks for the clauses that may contain it, and once
    // more under its first, where a visited wider clause finds each clause it may
    // contain only once.
    std::vector<std::vector<ClauseId>> by_leading_col(constant_col_idx);
    std::vector<std::vector<ClauseId>> by_first_leading_col(constant_col_idx);
    std::vector<ClauseId> visits;
    // A scan reads these by id before it reads a clause: its width, and its leading
    // columns folded into one word, which a containment must also hold for.
    std::vector<std::size_t> width_of(clause_database_.num_clauses(), 0);
    std::vector<std::uint64_t> leading_col_signature(clause_database_.num_clauses(), 0);
    std::vector<std::uint8_t> is_changed(clause_database_.num_clauses(), 0);
    // A clause the pass strengthened in place takes no further part in it, its
    // entries being those of the clause it was.
    std::vector<std::uint8_t> is_strengthened(clause_database_.num_clauses(), 0);
    const auto takes_part = [&](const ClauseId id) {
        return clause_database_.is_clause_active(id) && is_strengthened[id] == 0;
    };
    for (const ClauseId id : clause_database_.active_clause_ids()) {
        const Clause& clause = clause_database_.clause(id);
        assert(!clause.pivot_col_idxs().test(constant_col_idx) &&
               "an active clause spans 0 = 1");
        width_of[id] = clause.rows();
        is_changed[id] = clause_database_.record(id).subsume_passed ? 0 : 1;
        const bool indexed = is_changed[id] != 0;
        bool first = true;
        clause.pivot_col_idxs().for_each_set([&](const std::size_t col_idx) {
            leading_col_signature[id] |= std::uint64_t{1} << (col_idx & 63U);
            if (indexed) {
                by_leading_col[col_idx].push_back(id);
                if (first) {
                    by_first_leading_col[col_idx].push_back(id);
                    first = false;
                }
            }
        });
        visits.push_back(id);
    }
    // Which partner a scan meets first is by a uniform draw, not by clause id.
    for (std::vector<ClauseId>& ids : by_leading_col) {
        shuffle_uniformly(ids, subsume_pass_rng_);
    }
    for (std::vector<ClauseId>& ids : by_first_leading_col) {
        shuffle_uniformly(ids, subsume_pass_rng_);
    }
    shuffle_uniformly(visits, subsume_pass_rng_);

    PairCheck check(repr_, active_num_vars_);
    const auto relate = [&](const Clause& narrower, const Clause& wider) {
        return check.relate(narrower, wider);
    };
    std::vector<std::pair<Equation, ClauseId>> units;

    // Act on a hit of `narrower_id` on `wider_id`.
    const auto apply = [&](const ClauseId narrower_id, const ClauseId wider_id,
                           const PairRelation relation) {
        if (relation == PairRelation::subsumes) {
            ++stats_.num_inproc_subsumed_clauses;
            redirect_clause_state(wider_id, narrower_id);
            remove_clause(wider_id);
            return;
        }
        ++stats_.num_inproc_strengthened_clauses;
        Clause strengthened =
            check.strengthened(clause_database_.clause(wider_id), subsume_pass_rng_);
        std::vector<ClauseId> hints;
        if (proof_writer_ != nullptr) {
            // The wider clause is unit on the row taken off, and the narrower one
            // is then falsified.
            hints = {proof_id_of_clause_[wider_id], proof_id_of_clause_[narrower_id]};
        }
        // A clause with no rows would be falsified at the root, where both clauses
        // it came from are satisfied or not yet unit after propagation.
        assert(strengthened.rows() != 0 && "a strengthening at a quiescent root made the empty clause");
        if (strengthened.rows() == 1) {
            // Fed once the pass is over; the unit subsumes the clause it came from.
            const ClauseId proof_line = write_proof_line(strengthened, hints);
            Equation unit = strengthened.row(0);
            unit.negate();
            units.emplace_back(std::move(unit), proof_line);
            remove_clause(wider_id);
            return;
        }
        is_strengthened[wider_id] =
            put_narrowing_in_place(wider_id, std::move(strengthened), hints) ? 1 : 0;
    };

    // The visited clause as the wider clause: the first indexed clause that
    // subsumes or strengthens it. Whether it found one.
    std::vector<std::size_t> leading_col_idxs;
    const auto visit_as_wider = [&](const ClauseId visited_id) {
        const Clause& visited = clause_database_.clause(visited_id);
        leading_col_idxs.clear();
        visited.pivot_col_idxs().for_each_set(
            [&](const std::size_t col_idx) { leading_col_idxs.push_back(col_idx); });
        shuffle_uniformly(leading_col_idxs, subsume_pass_rng_);
        for (const std::size_t col_idx : leading_col_idxs) {
            for (const ClauseId narrower_id : by_first_leading_col[col_idx]) {
                if (narrower_id == visited_id || !takes_part(narrower_id) ||
                    width_of[narrower_id] > width_of[visited_id]) {
                    continue;
                }
                const Clause& narrower = clause_database_.clause(narrower_id);
                if ((leading_col_signature[narrower_id] & ~leading_col_signature[visited_id]) != 0 ||
                    !narrower.pivot_col_idxs().is_subset_of(visited.pivot_col_idxs())) {
                    continue;
                }
                const PairRelation relation = relate(narrower, visited);
                if (relation != PairRelation::none) {
                    apply(narrower_id, visited_id, relation);
                    return true;
                }
            }
        }
        return false;
    };

    // The visited clause as the narrower clause: every indexed clause it subsumes
    // or strengthens.
    const auto visit_as_narrower = [&](const ClauseId visited_id) {
        const Clause& visited = clause_database_.clause(visited_id);
        std::size_t rarest_col_idx = constant_col_idx;
        visited.pivot_col_idxs().for_each_set([&](const std::size_t col_idx) {
            if (rarest_col_idx == constant_col_idx ||
                by_leading_col[col_idx].size() < by_leading_col[rarest_col_idx].size()) {
                rarest_col_idx = col_idx;
            }
        });
        for (const ClauseId wider_id : by_leading_col[rarest_col_idx]) {
            if (wider_id == visited_id || !takes_part(wider_id) ||
                is_locked.test(wider_id) || width_of[wider_id] < width_of[visited_id]) {
                continue;
            }
            const Clause& wider = clause_database_.clause(wider_id);
            if ((leading_col_signature[visited_id] & ~leading_col_signature[wider_id]) != 0 ||
                !visited.pivot_col_idxs().is_subset_of(wider.pivot_col_idxs())) {
                continue;
            }
            const PairRelation relation = relate(visited, wider);
            if (relation != PairRelation::none) {
                apply(visited_id, wider_id, relation);
            }
        }
    };

    // Every clause as the narrower clause, narrowest first, which meets each
    // changed wider clause; then each unchanged one as the wider clause, which
    // meets each changed narrower clause it does not contain already. A changed
    // clause the budget leaves unvisited as the narrower clause stays changed.
    const auto out_of_budget = [&] {
        return static_cast<double>(check.num_rows_read()) >= budget;
    };
    std::vector<ClauseId> narrowest_first = visits;
    std::stable_sort(narrowest_first.begin(), narrowest_first.end(),
                     [&](const ClauseId a, const ClauseId b) {
                         return width_of[a] < width_of[b];
                     });
    bool stopped = false;
    for (const ClauseId visited_id : narrowest_first) {
        if (!takes_part(visited_id)) {
            continue;
        }
        if (out_of_budget()) {
            stopped = true;
            break;
        }
        visit_as_narrower(visited_id);
        if (is_changed[visited_id] != 0 && !is_locked.test(visited_id)) {
            clause_database_.record(visited_id).subsume_passed = true;
        }
    }
    for (std::size_t i = 0; i < visits.size() && !stopped; ++i) {
        const ClauseId visited_id = visits[i];
        if (is_changed[visited_id] != 0 || is_locked.test(visited_id) ||
            !takes_part(visited_id)) {
            continue;
        }
        if (out_of_budget()) {
            stopped = true;
            break;
        }
        visit_as_wider(visited_id);
    }

    assert(active_clause_count_is_consistent() &&
           "a subsumption pass left the live-clause count wrong");
    assert(debug_equivalent_clause_table_is_exact() &&
           "the equivalent clause table is out of step with the active clauses");
    assert(debug_no_two_clauses_share_a_span() && "two active clauses have one span");

    // A unit can already hold at the root, or follow from an earlier one.
    feed_units(units, trail_.span());
}

} // namespace xorcle
