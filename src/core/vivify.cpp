/// @file vivify.cpp
/// @brief Vivification: narrowing kept clauses, narrowest first, by assuming rows
/// of their linear negation on the solver's own trail.

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <random>
#include <span>
#include <utility>
#include <vector>

#include "algebra/equation.h"
#include "algebra/reduce_result.h"
#include "algebra/row_echelon_matrix.h"
#include "core/solver.h"
#include "heuristics/compaction/compaction.h"
#include "util/index_set.h"
#include "util/random.h"

namespace xorcle {

namespace {

/// @brief Stands in a candidate list for a clause narrowed to a unit.
constexpr ClauseId no_clause_id = SIZE_MAX;

/// @brief The narrowest clauses a pass tries; width 2 is the implication graph's.
constexpr std::size_t vivify_min_width = 3;

/// @brief A uniform draw from the 2^53 multiples of 2^-53 below 1.
double uniform_below_one(Rng& rng) {
    return static_cast<double>(rng() >> 11) * 0x1.0p-53;
}

template <typename T>
void shuffle_uniformly(std::vector<T>& items, Rng& rng) {
    for (std::size_t i = items.size(); i > 1; --i) {
        std::uniform_int_distribution<std::size_t> below(0, i - 1);
        std::swap(items[i - 1], items[below(rng)]);
    }
}

} // namespace

void Solver::vivify_assume(const Equation& reduced_eqn) {
    trail_.learn(reduced_eqn, std::nullopt, trail_.level() + 1);
    level_stamp_.resize(trail_.level() + 1);
    level_stamp_[trail_.level()] = next_level_stamp_++;
    decision_clause_of_level_.emplace_back(std::nullopt);
    selector_->update_with_decision(reduced_eqn, trail_);
    if (cyclic_selector_ != nullptr) {
        cyclic_selector_->update_with_decision_clause(std::nullopt);
    }
}

bool Solver::vivify_is_narrowing(const Clause& narrowed, const ClauseId id) const {
    assert(trail_.level() == 0 && "a narrowing is told at the root");
    const RowSpace& root_rows = trail_.span();
    RowSpace clause_rows(repr_, active_num_vars_);
    Equation reduced(repr_, active_num_vars_);
    for (const Equation& row : selector_->clause(id)) {
        reduced.assign_from(row);
        root_rows.reduce(reduced);
        clause_rows.add_row(reduced);
    }
    for (const Equation& row : narrowed) {
        reduced.assign_from(row);
        root_rows.reduce(reduced);
        clause_rows.reduce(reduced);
        if (!reduced.is_eq_true()) {
            return false;
        }
    }
    return true;
}

std::vector<ClauseId> Solver::vivify_proof_hints(const Clause& narrowed,
                                                 const std::size_t last_level) const {
    RowSpace implied(repr_, active_num_vars_);
    for (const Equation& row : narrowed) {
        implied.add_row(row);
    }
    std::vector<ClauseId> hints;
    Equation reduced(repr_, active_num_vars_);
    Equation unit(repr_, active_num_vars_);
    // Whether the line conflicts, else adds the one equation it implies, as the checker does.
    const auto replay = [&](const auto& rows) {
        bool conflicts = true;
        for (const Equation& row : rows) {
            reduced.assign_from(row);
            implied.reduce(reduced);
            if (reduced.is_eq_true()) {
                continue;
            }
            if (conflicts) {
                unit.assign_from(reduced);
                conflicts = false;
            } else {
                assert(reduced == unit && "a reason on the trail is not unit under what it implies");
            }
        }
        if (conflicts) {
            return true;
        }
        unit.plus_assign(false_equation_);
        implied.reduce(unit);
        if (!unit.is_eq_true()) {
            implied.add_row(unit);
        }
        return false;
    };
    const std::size_t num_rows = trail_.rows_at_level(last_level);
    for (std::size_t trail_idx = 0; trail_idx < num_rows; ++trail_idx) {
        const std::optional<ClauseId>& reason = trail_.reasons()[trail_idx];
        if (!reason.has_value()) {
            continue; // An assumed row, which the linear negation holds.
        }
        const std::size_t rows_before = implied.rows();
        bool conflicts = false;
        ClauseId hint = 0;
        if (trail_.level_of(trail_idx) == 0) {
            // The root unit's own line, whose row is this one reduced by the units before it.
            Equation negated = trail_.row(trail_idx);
            negated.plus_assign(false_equation_);
            conflicts = replay(std::span<const Equation>(&negated, 1));
            hint = proof_root_units_[trail_idx].id;
        } else {
            conflicts = replay(selector_->clause(*reason));
            hint = proof_id_of_clause_[*reason];
        }
        if (conflicts || implied.rows() != rows_before) {
            hints.push_back(hint);
        }
        if (conflicts) {
            return hints;
        }
    }
    assert(false && "the trail does not refute the narrowed clause");
    return hints;
}

std::optional<VivifyNarrowing> Solver::vivify_one_order(const ClauseId id) {
    assert(trail_.level() == 0 && "an order starts at the root");
    ++vivify_counters_.num_orders;
#ifndef NDEBUG
    const std::size_t debug_root_rows = trail_.span().rows();
#endif

    // The store may move while the trail is built on, so the rows are copied.
    std::vector<Equation> rows;
    for (const Equation& row : selector_->clause(id)) {
        rows.push_back(row);
    }
    const std::size_t width = rows.size();
    assert(width >= 2 && "a unit clause has nothing to narrow");

    const auto to_clause = [this](const std::vector<Equation>& kept_rows) {
        Clause clause(repr_, active_num_vars_);
        for (const Equation& row : kept_rows) {
            clause.add_row(row);
        }
        return clause;
    };

    // What assuming one more row of the linear negation found.
    enum class Step : std::uint8_t { implied, assumed, refuted, conflicting };
    std::vector<Equation> assumed; // By level less one.
    std::size_t refuted_level = 0;
    Equation reduced(repr_, active_num_vars_);
    const auto assume = [&](const Equation& row) {
        reduced.assign_from(row);
        const ReduceResult::Kind kind = trail_.span().partial_reduce(reduced).kind;
        if (kind == ReduceResult::Kind::eq_true) {
            // Implied by the rows assumed so far, so it falls out of the clause.
            return Step::implied;
        }
        if (kind == ReduceResult::Kind::eq_false) {
            refuted_level = shallowest_satisfying_level(row);
            return Step::refuted;
        }
        vivify_assume(reduced);
        ++vivify_counters_.num_assumed;
        assumed.push_back(row);
        return propagate_until_conflict() ? Step::conflicting : Step::assumed;
    };

    // A uniformly random ordered basis, drawn a row at a time.
    RowEchelonMatrix drawn(repr_, width);
    Equation coefficients = Equation::with_num_bits(repr_, width);
    std::vector<std::size_t> coefficient_bits;
    Equation last_row(repr_, active_num_vars_);
    Step step = Step::implied;
    for (std::size_t drawn_rows = 0;
         drawn_rows < width && (step == Step::implied || step == Step::assumed); ++drawn_rows) {
        while (true) {
            const IndexSet drawn_coefficients = random_nonzero_coefficients(width, vivify_rng_);
            coefficient_bits.clear();
            drawn_coefficients.for_each_set(
                [&](const std::size_t row_idx) { coefficient_bits.push_back(row_idx); });
            coefficients.assign_bits(coefficient_bits);
            if (drawn.add_row_get_row_idx(coefficients).has_value()) {
                break;
            }
        }
        last_row.set_eq_true();
        for (const std::size_t row_idx : coefficient_bits) {
            last_row.plus_assign(rows[row_idx]);
        }
        step = assume(last_row);
    }
    // Once every row is implied the clause itself conflicts.
    assert((step == Step::refuted || step == Step::conflicting) &&
           "an order implied the clause's every row without a conflict");
    if (step == Step::refuted && refuted_level == 0) {
        ++vivify_counters_.num_root_satisfied;
        if (trail_.level() != 0) {
            backjump_to(0);
        }
        return std::nullopt;
    }

    // On a conflict the rows assumed; on a refuted row the rows assumed up to the
    // level refuting it, with that row.
    const auto rows_reached = [&] {
        std::vector<Equation> kept = assumed;
        if (step == Step::refuted) {
            kept.resize(refuted_level);
            kept.push_back(last_row);
        }
        return kept;
    };
    std::optional<VivifyNarrowing> result;
    bool result_by_refutation = false;
    const auto take = [&](const std::vector<Equation>& kept) {
        if (kept.size() >= (result.has_value() ? result->clause.rows() : width)) {
            return;
        }
        Clause narrowed = to_clause(kept);
        std::vector<ClauseId> hints;
        if (proof_writer_ != nullptr) {
            hints = vivify_proof_hints(narrowed,
                                       step == Step::refuted ? refuted_level : trail_.level());
        }
        result = VivifyNarrowing{.clause = std::move(narrowed), .proof_hints = std::move(hints)};
        result_by_refutation = step == Step::refuted;
    };
    std::vector<Equation> kept = rows_reached();
    take(kept);

    if (result.has_value()) {
        ++(result_by_refutation ? vivify_counters_.num_narrowed_by_refutation
                                : vivify_counters_.num_narrowed_by_conflict);
    }
    if (trail_.level() != 0) {
        backjump_to(0);
    }
    assert(trail_.span().rows() == debug_root_rows && "an order left rows on the root trail");
    return result;
}

std::optional<ClauseId> Solver::vivify_replace(
    const ClauseId id, VivifyNarrowing narrowing,
    std::vector<std::pair<Equation, ClauseId>>& units) {
    Clause& narrowed = narrowing.clause;
    narrowed.reduce_rows();
    const std::size_t width = selector_->clause(id).rows();
    assert(vivify_is_narrowing(narrowed, id) &&
           "a clause was to be replaced by one that does not imply it");
    ++vivify_counters_.num_narrowed;
    vivify_counters_.narrowed_width_before += width;
    vivify_counters_.narrowed_width_after += narrowed.rows();
    const auto write_line = [&]() -> ClauseId {
        if (proof_writer_ == nullptr) {
            return 0;
        }
        const ClauseId line = next_proof_clause_id_++;
        proof_writer_->write_learned(line, narrowed, active_var_to_original_,
                                     narrowing.proof_hints);
        return line;
    };
    if (narrowed.rows() == 1) {
        // Fed once the pass is over: a unit met above the root would be learned there.
        const ClauseId proof_line = write_line();
        Equation unit = narrowed.row(0);
        unit.plus_assign(false_equation_);
        units.emplace_back(std::move(unit), proof_line);
        ++vivify_counters_.num_units;
        return std::nullopt;
    }
    vivify_counters_.num_narrowed_to_width_2 += width > 2 && narrowed.rows() == 2 ? 1U : 0U;

    const EvictedClauseState replaced = evicted_clause_state(id);
    equivalent_clauses_.remove(id);
    std::optional<ClauseId> replacement_id = selector_->num_clauses();
    const std::optional<ClauseId> kept_id = equivalent_clauses_.find_or_enter(
        narrowed, narrowed.rows_hash(active_var_to_original_), *replacement_id,
        selector_->clauses());
    if (kept_id.has_value()) {
        // A kept clause already has the narrowing's span, so none is added.
        merge_into_equivalent_clause(*kept_id, replaced,
                                     &SolverCounters::num_equivalent_clauses_at_vivification);
        decisions_->update_with_subsumed_clause(id, *kept_id);
        replacement_id.reset();
    } else {
        const ClauseId proof_line = write_line();
        decisions_->update_with_new_clause(narrowed, *replacement_id);
        if (proof_writer_ != nullptr) {
            assert(proof_id_of_clause_.size() == *replacement_id &&
                   "a clause went without a proof id");
            proof_id_of_clause_.push_back(proof_line);
        }
        selector_->add_asserting_clause(
            std::move(narrowed), trail_,
            AssertingClauseInfo{.lbd = 0, .removed_levels = 0, .is_replacement = true});
        ++num_active_clauses_;
        if (replaced.is_learned) {
            selector_->set_clause_lbd(*replacement_id, replaced.lbd);
        } else {
            selector_->set_clause_learned(*replacement_id, false);
        }
        selector_->set_clause_unused_passes(*replacement_id, replaced.unused_passes);
        selector_->set_clause_out_of_unused_passes(*replacement_id,
                                                   replaced.is_out_of_unused_passes);
        selector_->set_clause_used_since_deletion(*replacement_id, replaced.is_used_since_deletion);
        decisions_->update_with_subsumed_clause(id, *replacement_id);
        is_vivify_replacement_.resize(*replacement_id + 1, 0);
        is_vivify_replacement_[*replacement_id] = 1;
    }

    if (proof_writer_ != nullptr) {
        proof_writer_->write_deleted({proof_id_of_clause_[id]});
    }
    selector_->set_clause_active(id, false);
    selector_->replace_clause(id, inactive_clause_placeholder(repr_, false_equation_));
    --num_active_clauses_;
    return replacement_id;
}

std::vector<std::vector<ClauseId>> Solver::vivify_candidates_by_width() {
    std::vector<ClauseId> locked;
    for (const std::optional<ClauseId>& reason : trail_.reasons()) {
        if (reason.has_value()) {
            locked.push_back(*reason);
        }
    }
    std::sort(locked.begin(), locked.end());

    std::vector<std::vector<ClauseId>> by_width(selector_->num_active_clauses_by_width().size());
    for (const ClauseId id : selector_->active_clause_ids()) {
        const std::size_t width = selector_->clause(id).rows();
        if (width < vivify_min_width) {
            continue;
        }
        if (selector_->is_clause_vivified(id)) {
            ++vivify_counters_.num_unchanged;
            continue;
        }
        if (!std::binary_search(locked.begin(), locked.end(), id)) {
            by_width[width].push_back(id);
        }
    }
    return by_width;
}

bool Solver::maybe_vivify() {
    if (!vivify_.enabled) {
        return false;
    }
    if (trail_.level() != 0) {
        vivify_passed_at_this_root_visit_ = false;
        return false;
    }
    if (counters_.num_conflicts - conflicts_at_last_vivify_pass_ < vivify_.cadence ||
        vivify_passed_at_this_root_visit_) {
        return false;
    }
    const std::span<const std::size_t> num_by_width = selector_->num_active_clauses_by_width();
    if (std::all_of(num_by_width.begin() + static_cast<std::ptrdiff_t>(std::min(
                                               vivify_min_width, num_by_width.size())),
                    num_by_width.end(), [](const std::size_t num) { return num == 0; })) {
        return false;
    }
    const auto start = std::chrono::steady_clock::now();
    const std::size_t search_rows = decisions_->counters().decisions + counters_.num_propagations;
    const double budget =
        vivify_.effort * static_cast<double>(search_rows - search_rows_at_last_vivify_pass_);
    conflicts_at_last_vivify_pass_ = counters_.num_conflicts;
    search_rows_at_last_vivify_pass_ = search_rows;
    vivify_passed_at_this_root_visit_ = true;

#ifndef NDEBUG
    selector_->debug_assert_widths_counted();
#endif
    std::vector<std::vector<ClauseId>> by_width = vivify_candidates_by_width();
    if (std::all_of(by_width.begin(), by_width.end(),
                    [](const std::vector<ClauseId>& ids) { return ids.empty(); })) {
        return false;
    }

    // The assumptions and what they imply are not the search's.
    const SolverCounters search_counters = counters_;
    ++vivify_counters_.num_passes;
    // A clause this pass puts in place has not had the rounds of its own width.
    const ClauseId first_replacement_id = selector_->num_clauses();

    // The width at which the budget stopped the pass.
    std::optional<std::size_t> stopped_at;
    const std::size_t assumed_before = vivify_counters_.num_assumed;
    const auto out_of_budget = [&] {
        const std::size_t spent = vivify_counters_.num_assumed - assumed_before +
                                  counters_.num_propagations - search_counters.num_propagations;
        return static_cast<double>(spent) >= budget;
    };

    std::vector<std::pair<Equation, ClauseId>> units;
    bool ran_an_order = false;
    for (std::size_t width = vivify_min_width; width < by_width.size() && !stopped_at; ++width) {
        // A clause that narrows keeps its place here under its new id.
        std::vector<ClauseId>& ids = by_width[width];
        vivify_counters_.num_candidates += ids.size();
        const double draw_below = std::pow(2.0, 3.0 - static_cast<double>(width));
        for (std::size_t round = 0; round < vivify_.rounds && !ids.empty() && !stopped_at;
             ++round) {
            shuffle_uniformly(ids, vivify_rng_);
            for (std::size_t i = 0; i < ids.size(); ++i) {
                const ClauseId id = ids[i];
                if (draw_below < 1.0 && uniform_below_one(vivify_rng_) >= draw_below) {
                    continue;
                }
                if (out_of_budget()) {
                    stopped_at = width;
                    break;
                }
                std::optional<VivifyNarrowing> narrowed = vivify_one_order(id);
                ran_an_order = true;
                if (narrowed.has_value()) {
                    ids[i] = vivify_replace(id, std::move(*narrowed), units).value_or(no_clause_id);
                } else if (id < first_replacement_id) {
                    selector_->set_clause_vivified(id);
                }
            }
            std::erase(ids, no_clause_id);
        }
    }
    // Every clause of a width the pass finished counts as gone through, a clause it
    // tried at the width the budget stopped it already does.
    for (std::size_t width = vivify_min_width;
         width < by_width.size() && (!stopped_at || width < *stopped_at); ++width) {
        for (const ClauseId id : by_width[width]) {
            if (id < first_replacement_id) {
                selector_->set_clause_vivified(id);
            }
        }
    }
    vivify_counters_.num_budget_stops += stopped_at ? 1U : 0U;

    assert(debug_equivalent_clause_table_is_exact() &&
           "the equivalent clause table is out of step with the active clauses");
    assert(debug_no_two_clauses_share_a_span() && "two active clauses have one span");

    // An earlier unit of this pass can already imply a later one.
    RowSpace fed(repr_, active_num_vars_);
    for (const auto& [unit, proof_line] : units) {
        if (fed.add_row_get_row_idx(unit).has_value()) {
            feed_unit(unit, proof_line);
        }
    }

    vivify_counters_.num_implied += counters_.num_propagations - search_counters.num_propagations;
    vivify_counters_.num_attempt_props +=
        counters_.num_attempt_props - search_counters.num_attempt_props;
    // The evictions are the pass's, and the search's counters keep them.
    SolverCounters kept_counters = search_counters;
    kept_counters.num_equivalent_clauses_at_vivification =
        counters_.num_equivalent_clauses_at_vivification;
    kept_counters.num_equivalent_starting_learned_pairs =
        counters_.num_equivalent_starting_learned_pairs;
    kept_counters.num_equivalent_learned_pairs = counters_.num_equivalent_learned_pairs;
    counters_ = kept_counters;
    const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start;
    vivify_counters_.time_s += elapsed.count();
    // The selector has not run at the root since the last order was undone.
    return ran_an_order;
}

bool Solver::maybe_force_vivify_root_visit() {
    // A replay under way, or no conflict since the last pass, leaves nothing new to visit for.
    if (trail_.level() == 0 || vivify_replay_next_ < vivify_replay_.size() ||
        counters_.num_conflicts == conflicts_at_last_vivify_pass_ ||
        counters_.num_conflicts - conflicts_at_last_vivify_pass_ < vivify_.cadence) {
        return false;
    }
    const std::span<const std::size_t> num_by_width = selector_->num_active_clauses_by_width();
    if (std::all_of(num_by_width.begin() + static_cast<std::ptrdiff_t>(std::min(
                                               vivify_min_width, num_by_width.size())),
                    num_by_width.end(), [](const std::size_t num) { return num == 0; })) {
        return false;
    }
    vivify_replay_.clear();
    vivify_replay_next_ = 0;
    for (std::size_t level = 1; level <= trail_.level(); ++level) {
        const std::size_t trail_idx = trail_.rows_at_level(level - 1);
        assert(!trail_.reasons()[trail_idx].has_value() && "a level opens with a propagated row");
        vivify_replay_.emplace_back(trail_.row(trail_idx), decision_clause_of_level_[level - 1]);
    }
    ++vivify_counters_.num_forced_visits;
    vivify_counters_.forced_visit_rows += trail_.span().rows() - trail_.rows_at_level(0);
    // The backjump can queue clauses, so the loop propagates the root before the pass.
    backjump_to(0);
    return true;
}

Solver::DecisionStep Solver::replay_decision_step() {
    while (vivify_replay_next_ < vivify_replay_.size()) {
        auto& [decision_eqn, decision_clause] = vivify_replay_[vivify_replay_next_++];
        if (trail_.span().partial_reduce(decision_eqn).kind != ReduceResult::Kind::leading) {
            ++vivify_counters_.num_replay_skipped;
            continue;
        }
        ++vivify_counters_.num_replayed;
        trail_.learn(decision_eqn, std::nullopt, trail_.level() + 1);
        level_stamp_.resize(trail_.level() + 1);
        level_stamp_[trail_.level()] = next_level_stamp_++;
        decision_clause_of_level_.push_back(decision_clause);
        selector_->update_with_decision(decision_eqn, trail_);
        if (cyclic_selector_ != nullptr) {
            cyclic_selector_->update_with_decision_clause(decision_clause);
        }
        decisions_->update_with_replayed_decision();
        return DecisionStep::decided;
    }
    vivify_replay_.clear();
    vivify_replay_next_ = 0;
    return make_decision_step();
}

} // namespace xorcle
