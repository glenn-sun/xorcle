#include <doctest/doctest.h>

#include <cstddef>
#include <initializer_list>
#include <optional>
#include <vector>

#include "algebra/equation.h"
#include "algebra/row_echelon_matrix.h"
#include "algebra_test_support.h"
#include "core/clause_id.h"
#include "core/propagation.h"
#include "core/trail.h"

namespace xorcle {

namespace {

/// @brief Number of variables every clause in this file is built over, unless
/// the test says otherwise.
constexpr std::size_t default_num_vars = 4;

/// @brief Build a clause from the set bits of each of its linear-negation rows.
Clause make_clause(EquationRepr repr, std::size_t clause_num_vars,
                   std::initializer_list<std::initializer_list<std::size_t>> rows) {
    Clause clause(repr, clause_num_vars);
    for (const std::initializer_list<std::size_t>& row : rows) {
        clause.add_row(make_equation_with(repr, clause_num_vars, row));
    }
    return clause;
}

/// @brief A `WatchUpdateHandler` that records every notification it receives.
class RecordingHandler final : public WatchUpdateHandler {
public:
    struct Record {
        ClauseId id; ///< The clause whose watch moved.
        ClauseRowIndex old_watch; ///< The row the watch pointed at before the move.
        WatchType type; ///< Which watch moved.
        std::size_t watched0; ///< The clause's `watched0` as seen by the handler.
        std::size_t watched1; ///< The clause's `watched1` as seen by the handler.
    };

    /// @brief Observe `clause`, so the recorded watches reflect the update order.
    explicit RecordingHandler(const Clause& clause) : clause_(&clause) {}

    void update_with_new_watch(ClauseId id, ClauseRowIndex old_watch, WatchType type,
                               const Trail&) override {
        records.push_back({id, old_watch, type, clause_->watched0, clause_->watched1});
    }

    std::vector<Record> records; ///< Every notification, in order.

private:
    const Clause* clause_;
};

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
    trail.learn(make_equation_with(repr, default_num_vars, {0, default_num_vars}), ClauseId{0});

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
    trail.learn(make_equation_with(repr, default_num_vars, {0}), ClauseId{0});

    // The row reduces to 0, so the clause implies 1: conflict and propagation
    // travel the same code path.
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

    trail.learn(make_equation_with(repr, default_num_vars, {0}), ClauseId{0});

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

    trail.learn(make_equation_with(repr, default_num_vars, {0}), ClauseId{0});
    trail.learn(make_equation_with(repr, default_num_vars, {1, default_num_vars}), ClauseId{1});

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
    trail.learn(make_equation_with(repr, default_num_vars, {0}), ClauseId{0});

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

    trail.learn(make_equation_with(repr, default_num_vars, {0}), ClauseId{0});
    trail.learn(make_equation_with(repr, default_num_vars, {1}), ClauseId{1});

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
    trail.learn(make_equation_with(repr, default_num_vars, {0}), ClauseId{0});
    trail.learn(make_equation_with(repr, default_num_vars, {1, 2}), ClauseId{1});

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

    trail.learn(make_equation_with(repr, default_num_vars, {0}), ClauseId{0});
    CHECK(propagator.unit_propagate_one_clause(3, clause, trail, std::nullopt, num_watch_updates,
                                               handler) == nullptr);
    CHECK(clause.watched0 == 2);
    CHECK(clause.next_watch_search_start == 3);

    // Watch 1 now dies. The search starts from the persisted position rather
    // than from row 0, so it settles on row 3.
    trail.learn(make_equation_with(repr, default_num_vars, {1}), ClauseId{1});
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
