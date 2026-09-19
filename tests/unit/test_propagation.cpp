#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <vector>

#include "algebra/equation.h"
#include "algebra/row_echelon_matrix.h"
#include "algebra_test_support.h"
#include "core/clause_id.h"
#include "core/propagation.h"
#include "core/trail.h"
#include "heuristics_test_support.h"
#include "util/index_set.h"

namespace xorcle {

namespace {

/// @brief Number of variables every clause in this file is built over, unless
/// the test says otherwise.
constexpr std::size_t default_num_vars = 4;

/// @brief A `WatchUpdateHandler` that records every notification it receives.
class RecordingHandler final : public WatchUpdateHandler {
public:
    struct Record {
        ClauseId id; ///< The clause whose watch moved.
        ClauseRowIdx old_watch; ///< The row the watch pointed at before the move.
        WatchType type; ///< Which watch moved.
        std::size_t watched0; ///< The clause's `watched0` as seen by the handler.
        std::size_t watched1; ///< The clause's `watched1` as seen by the handler.
    };

    /// @brief Observe `clause`, so the recorded watches reflect the update order.
    explicit RecordingHandler(const Clause& clause) : clause_(&clause) {}

    void update_with_new_watch(ClauseId id, ClauseRowIdx old_watch, WatchType type,
                               const Trail&) override {
        records.push_back({id, old_watch, type, clause_->watched0, clause_->watched1});
    }

    std::vector<Record> records; ///< Every notification, in order.

private:
    const Clause* clause_;
};

/// @brief The deepest level among the trail rows `used_row_idxs` names, which is
/// the level the solver dates a propagation to.
std::size_t level_of_used_rows(const Trail& trail, const IndexSet& used_row_idxs) {
    std::size_t level = 0;
    used_row_idxs.for_each_set([&](const std::size_t row_idx) {
        level = std::max(level, trail.level_of(trail.row_idx_to_trail_idx()[row_idx]));
    });
    return level;
}

} // namespace

TEST_CASE_TEMPLATE("propagation: unit clause propagates its linear negation", Tag, DenseTag,
                   SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    Propagator propagator(repr, default_num_vars);
    Trail trail(repr, default_num_vars);
    IgnoreWatchUpdates handler;
    std::size_t num_watch_updates = 0;

    // The clause [x0 = 1] is stored as its linear negation [x0 = 0].
    Clause clause = make_clause(repr, default_num_vars, {{0}});
    REQUIRE(clause.rows() == 1);

    Equation* implied = propagator.unit_propagate_one_clause(0, clause, trail, std::nullopt,
                                                             num_watch_updates, handler);
    REQUIRE(implied != nullptr);
    CHECK(bits_of(*implied) == std::vector<std::size_t>{0, default_num_vars});
    CHECK(num_watch_updates == 0);
}

TEST_CASE_TEMPLATE("propagation: unit clause already satisfied", Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    Propagator propagator(repr, default_num_vars);
    Trail trail(repr, default_num_vars);
    IgnoreWatchUpdates handler;
    std::size_t num_watch_updates = 0;

    Clause clause = make_clause(repr, default_num_vars, {{0}});
    trail.learn(make_equation_with(repr, default_num_vars, {0, default_num_vars}), ClauseId{0},
                trail.level());

    // The row reduces to 1, so its negation is 0: nothing to propagate.
    CHECK(propagator.unit_propagate_one_clause(0, clause, trail, std::nullopt, num_watch_updates,
                                               handler) == nullptr);
}

TEST_CASE_TEMPLATE("propagation: unit clause conflict is an implied false equation", Tag, DenseTag,
                   SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    Propagator propagator(repr, default_num_vars);
    Trail trail(repr, default_num_vars);
    IgnoreWatchUpdates handler;
    std::size_t num_watch_updates = 0;

    Clause clause = make_clause(repr, default_num_vars, {{0}});
    trail.learn(make_equation_with(repr, default_num_vars, {0}), ClauseId{0}, trail.level());

    // The row reduces to 0, so the clause implies 1.
    Equation* implied = propagator.unit_propagate_one_clause(0, clause, trail, std::nullopt,
                                                             num_watch_updates, handler);
    REQUIRE(implied != nullptr);
    CHECK(implied->is_eq_false());
}

TEST_CASE_TEMPLATE("propagation: two-row clause with both watches alive", Tag, DenseTag,
                   SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    Propagator propagator(repr, default_num_vars);
    Trail trail(repr, default_num_vars);
    Clause clause = make_clause(repr, default_num_vars, {{0}, {1}});
    RecordingHandler handler(clause);
    std::size_t num_watch_updates = 0;

    CHECK(propagator.unit_propagate_one_clause(3, clause, trail, std::nullopt, num_watch_updates,
                                               handler) == nullptr);
    CHECK(clause.watched0 == 0);
    CHECK(clause.watched1 == 1);
    CHECK(num_watch_updates == 0);
    CHECK(handler.records.empty());
}

TEST_CASE_TEMPLATE("propagation: two-row clause propagates its complement watch", Tag, DenseTag,
                   SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    Propagator propagator(repr, default_num_vars);
    Trail trail(repr, default_num_vars);
    Clause clause = make_clause(repr, default_num_vars, {{0}, {1}});
    RecordingHandler handler(clause);
    std::size_t num_watch_updates = 0;

    trail.learn(make_equation_with(repr, default_num_vars, {0}), ClauseId{0}, trail.level());

    // Watch 0 died and there is no third row to move it to, so the clause
    // implies the negation of watch 1.
    Equation* implied = propagator.unit_propagate_one_clause(3, clause, trail, std::nullopt,
                                                             num_watch_updates, handler);
    REQUIRE(implied != nullptr);
    CHECK(bits_of(*implied) == std::vector<std::size_t>{1, default_num_vars});
    CHECK(num_watch_updates == 0);
    CHECK(clause.watched0 == 0);
    CHECK(clause.watched1 == 1);
}

TEST_CASE_TEMPLATE("propagation: clause satisfied through the complement watch", Tag, DenseTag,
                   SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    Propagator propagator(repr, default_num_vars);
    Trail trail(repr, default_num_vars);
    Clause clause = make_clause(repr, default_num_vars, {{0}, {1}});
    RecordingHandler handler(clause);
    std::size_t num_watch_updates = 0;

    trail.learn(make_equation_with(repr, default_num_vars, {0}), ClauseId{0}, trail.level());
    trail.learn(make_equation_with(repr, default_num_vars, {1, default_num_vars}), ClauseId{1},
                trail.level());

    // Watch 0 died, but watch 1 reduces to 1, so the clause is satisfied and
    // nothing is implied.
    CHECK(propagator.unit_propagate_one_clause(3, clause, trail, std::nullopt, num_watch_updates,
                                               handler) == nullptr);
    CHECK(num_watch_updates == 0);
    CHECK(handler.records.empty());
}

TEST_CASE_TEMPLATE("propagation: a dead watch moves to a live row", Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    Propagator propagator(repr, default_num_vars);
    Trail trail(repr, default_num_vars);
    Clause clause = make_clause(repr, default_num_vars, {{0}, {1}, {2}});
    RecordingHandler handler(clause);
    std::size_t num_watch_updates = 0;

    REQUIRE(clause.rows() == 3);
    trail.learn(make_equation_with(repr, default_num_vars, {0}), ClauseId{0}, trail.level());

    CHECK(propagator.unit_propagate_one_clause(3, clause, trail, std::nullopt, num_watch_updates,
                                               handler) == nullptr);
    CHECK(clause.watched0 == 2);
    CHECK(clause.watched1 == 1);
    CHECK(num_watch_updates == 1);

    // The cyclic search resumes just past the row it settled on.
    CHECK(clause.next_watch_search_start == 0);

    REQUIRE(handler.records.size() == 1);
    CHECK(handler.records[0].id == 3);
    CHECK(handler.records[0].old_watch == 0);
    CHECK(handler.records[0].type == WatchType::zero);
    // The clause is updated before the handler is called, so the handler sees
    // the new row while being handed the old one.
    CHECK(handler.records[0].watched0 == 2);
    CHECK(handler.records[0].watched1 == 1);
}

TEST_CASE_TEMPLATE("propagation: two tasks run against the already-updated watch", Tag, DenseTag,
                   SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    Propagator propagator(repr, default_num_vars);
    Trail trail(repr, default_num_vars);
    Clause clause = make_clause(repr, default_num_vars, {{0}, {1}, {2}});
    RecordingHandler handler(clause);
    std::size_t num_watch_updates = 0;

    trail.learn(make_equation_with(repr, default_num_vars, {0}), ClauseId{0}, trail.level());
    trail.learn(make_equation_with(repr, default_num_vars, {1}), ClauseId{1}, trail.level());

    // Both watches died, so two tasks are scheduled. The first moves watch 0 to
    // row 2; the second then finds no row left, because it runs against the
    // *updated* watch 0, and propagates the negation of row 2.
    Equation* implied = propagator.unit_propagate_one_clause(3, clause, trail, std::nullopt,
                                                             num_watch_updates, handler);
    REQUIRE(implied != nullptr);
    CHECK(bits_of(*implied) == std::vector<std::size_t>{2, default_num_vars});
    CHECK(clause.watched0 == 2);
    CHECK(clause.watched1 == 1);
    CHECK(num_watch_updates == 1);

    REQUIRE(handler.records.size() == 1);
    CHECK(handler.records[0].old_watch == 0);
    CHECK(handler.records[0].type == WatchType::zero);
}

TEST_CASE_TEMPLATE("propagation: watches must stay independent, not merely nonzero", Tag, DenseTag,
                   SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    Propagator propagator(repr, default_num_vars);
    Trail trail(repr, default_num_vars);

    // Rows 1 and 2 are equal modulo the span once x1 + x2 = 0 is known, so row 2
    // is not a legal replacement for a watch paired with row 1 even though it
    // reduces to something nonzero. Row 3 is.
    Clause clause = make_clause(repr, default_num_vars, {{0}, {1}, {2}, {3}});
    RecordingHandler handler(clause);
    std::size_t num_watch_updates = 0;

    REQUIRE(clause.rows() == 4);
    trail.learn(make_equation_with(repr, default_num_vars, {0}), ClauseId{0}, trail.level());
    trail.learn(make_equation_with(repr, default_num_vars, {1, 2}), ClauseId{1}, trail.level());

    CHECK(propagator.unit_propagate_one_clause(3, clause, trail, std::nullopt, num_watch_updates,
                                               handler) == nullptr);
    CHECK(clause.watched0 == 3);
    CHECK(clause.watched1 == 1);
    CHECK(num_watch_updates == 1);
    CHECK(clause.next_watch_search_start == 0);
}

TEST_CASE_TEMPLATE("propagation: the cyclic search start persists across calls", Tag, DenseTag,
                   SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    Propagator propagator(repr, default_num_vars);
    Trail trail(repr, default_num_vars);
    Clause clause = make_clause(repr, default_num_vars, {{0}, {1}, {2}, {3}});
    RecordingHandler handler(clause);
    std::size_t num_watch_updates = 0;

    trail.learn(make_equation_with(repr, default_num_vars, {0}), ClauseId{0}, trail.level());
    CHECK(propagator.unit_propagate_one_clause(3, clause, trail, std::nullopt, num_watch_updates,
                                               handler) == nullptr);
    CHECK(clause.watched0 == 2);
    CHECK(clause.next_watch_search_start == 3);

    // Watch 1 now dies. The search starts from the persisted position rather
    // than from row 0, so it settles on row 3.
    trail.learn(make_equation_with(repr, default_num_vars, {1}), ClauseId{1}, trail.level());
    CHECK(propagator.unit_propagate_one_clause(3, clause, trail, std::nullopt, num_watch_updates,
                                               handler) == nullptr);
    CHECK(clause.watched0 == 2);
    CHECK(clause.watched1 == 3);
    CHECK(clause.next_watch_search_start == 0);
    CHECK(num_watch_updates == 2);

    REQUIRE(handler.records.size() == 2);
    CHECK(handler.records[1].old_watch == 1);
    CHECK(handler.records[1].type == WatchType::one);
}

TEST_CASE_TEMPLATE("propagation: a row equal to the propagating one is dated by their sum", Tag,
                   DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    Propagator propagator(repr, default_num_vars);
    Trail trail(repr, default_num_vars);
    IgnoreWatchUpdates handler;
    std::size_t num_watch_updates = 0;

    trail.learn(make_equation_with(repr, default_num_vars, {3}), std::nullopt, 1);
    trail.learn(make_equation_with(repr, default_num_vars, {0, 1}), std::nullopt, 2);

    // Neither row is falsified, and neither uses the level-2 row on its own:
    // what the level-2 row falsifies is their sum.
    Clause clause = make_clause(repr, default_num_vars, {{0, 2}, {1, 2}});
    IndexSet used_row_idxs;
    Equation* implied = propagator.unit_propagate_one_clause(0, clause, trail, std::nullopt,
                                                             num_watch_updates, handler,
                                                             &used_row_idxs);
    REQUIRE(implied != nullptr);
    CHECK(bits_of(*implied) == std::vector<std::size_t>{1, 2, default_num_vars});
    CHECK(propagator.last_propagating_row_idx() == 0);
    CHECK(level_of_used_rows(trail, used_row_idxs) == 2);
}

TEST_CASE_TEMPLATE("propagation: the propagating row's own reduction dates nothing", Tag, DenseTag,
                   SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    Propagator propagator(repr, default_num_vars);
    Trail trail(repr, default_num_vars);
    IgnoreWatchUpdates handler;
    std::size_t num_watch_updates = 0;

    trail.learn(make_equation_with(repr, default_num_vars, {0}), std::nullopt, 1);
    trail.learn(make_equation_with(repr, default_num_vars, {1}), std::nullopt, 2);

    // Only row 1 is falsified, and the level-1 row is what falsifies it, so the
    // clause was already unit there.
    Clause clause = make_clause(repr, default_num_vars, {{0, 1, 2, default_num_vars}, {0}});
    IndexSet used_row_idxs;
    Equation* implied = propagator.unit_propagate_one_clause(0, clause, trail, std::nullopt,
                                                             num_watch_updates, handler,
                                                             &used_row_idxs);
    REQUIRE(implied != nullptr);
    CHECK(bits_of(*implied) == std::vector<std::size_t>{2});
    REQUIRE(propagator.last_propagating_row_idx() == 0);
    CHECK(level_of_used_rows(trail, used_row_idxs) == 1);

    // The raw row is what the solver learns, so that the trail reduces it by
    // level 1 alone and re-reduces the level-2 decision by the result.
    Equation raw = clause.row(propagator.last_propagating_row_idx());
    raw.plus_assign(Equation::eq_false(repr, default_num_vars));
    CHECK(trail.learn(raw, ClauseId{0}, 1) == 1);

    CHECK(bits_of(trail.span().row(trail.trail_idx_to_row_idx()[1])) ==
          std::vector<std::size_t>{1, 2});
    CHECK(bits_of(trail.span().row(trail.trail_idx_to_row_idx()[2])) ==
          std::vector<std::size_t>{2});
}

TEST_CASE_TEMPLATE("propagation: reconfigure resizes the scratch", Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    Propagator propagator(repr, default_num_vars);
    propagator.reconfigure(2);

    Trail trail(repr, 2);
    IgnoreWatchUpdates handler;
    std::size_t num_watch_updates = 0;
    Clause clause = make_clause(repr, 2, {{1}});

    Equation* implied = propagator.unit_propagate_one_clause(0, clause, trail, std::nullopt,
                                                             num_watch_updates, handler);
    REQUIRE(implied != nullptr);
    CHECK(implied->num_bits() == 3);
    CHECK(bits_of(*implied) == std::vector<std::size_t>{1, 2});
}

} // namespace xorcle
