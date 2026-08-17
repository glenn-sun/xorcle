#include <doctest/doctest.h>

#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "algebra/compaction_transform.h"
#include "algebra/equation.h"
#include "algebra/reduce_result.h"
#include "algebra_test_support.h"
#include "allocation_counter.h"
#include "util/bitmap.h"
#include "util/print_mode.h"

#if !defined(NDEBUG) && (defined(__APPLE__) || defined(__unix__))
#define XORCLE_HAS_DEATH_TESTS 1
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace xorcle {

TEST_CASE_TEMPLATE("equation: construction and width", Tag, DenseTag, SparseTag) {
    constexpr Algebra backend = Tag::value;

    Equation null_handle;
    CHECK(null_handle.empty());
    CHECK(null_handle.size() == 0);

    Equation equation(backend, 5);
    CHECK_FALSE(equation.empty());
    CHECK(equation.backend() == backend);
    CHECK(equation.num_vars() == 5);
    CHECK(equation.size() == 6);
    CHECK(equation.is_eq_true());
    CHECK(columns_of(equation).empty());

    equation.resize(9);
    CHECK(equation.num_vars() == 9);
    CHECK(equation.size() == 10);
    CHECK(equation.is_eq_true());
}

TEST_CASE_TEMPLATE("equation: eq_true, eq_false and classify", Tag, DenseTag, SparseTag) {
    constexpr Algebra backend = Tag::value;
    constexpr std::size_t num_vars = 130; // spans three words in the dense backend

    const Equation eq_true = Equation::eq_true(backend, num_vars);
    CHECK(eq_true.classify() == ReduceResult::eq_true());
    CHECK(eq_true.is_eq_true());
    CHECK_FALSE(eq_true.is_eq_false());
    CHECK(eq_true.leading_column() == std::nullopt);

    const Equation eq_false = Equation::eq_false(backend, num_vars);
    CHECK(eq_false.classify() == ReduceResult::eq_false());
    CHECK(eq_false.is_eq_false());
    CHECK_FALSE(eq_false.is_eq_true());
    CHECK(columns_of(eq_false) == std::vector<std::size_t>{num_vars});
    CHECK(eq_false.leading_column() == std::optional<std::size_t>(num_vars));

    SUBCASE("the case the old none_or_final() fell through on") {
        // `bit::vector::none_or_final()` returned None for the all-zero row and
        // Final for the constant column alone, and fell off the end of the
        // function -- undefined behaviour -- for every other row whose final
        // block was non-zero. Both shapes below hit that path.

        // Only a variable column set, in the final block.
        const Equation only_variable = make_equation_with(backend, num_vars, {129});
        CHECK(only_variable.classify() == ReduceResult::leading(129));
        CHECK(only_variable.classify().is_leading());
        CHECK_FALSE(only_variable.is_eq_true());
        CHECK_FALSE(only_variable.is_eq_false());

        // A variable column and the constant column together.
        const Equation with_constant = make_equation_with(backend, num_vars, {129, num_vars});
        CHECK(with_constant.classify() == ReduceResult::leading(129));
        CHECK_FALSE(with_constant.is_eq_true());
        CHECK_FALSE(with_constant.is_eq_false());

        // A variable column in an earlier block, so the leading column is not
        // in the final block at all.
        const Equation early = make_equation_with(backend, num_vars, {3, 129, num_vars});
        CHECK(early.classify() == ReduceResult::leading(3));
    }
}

TEST_CASE_TEMPLATE("equation: columns and leading_column", Tag, DenseTag, SparseTag) {
    constexpr Algebra backend = Tag::value;
    const Equation equation = make_equation_with(backend, 200, {0, 63, 64, 127, 199, 200});

    CHECK(columns_of(equation) == std::vector<std::size_t>{0, 63, 64, 127, 199, 200});
    CHECK(equation.leading_column() == std::optional<std::size_t>(0));

    // `columns()` clears the caller's buffer first, so a reused buffer is safe.
    std::vector<std::size_t> reused{99, 98, 97};
    equation.columns(reused);
    CHECK(reused == std::vector<std::size_t>{0, 63, 64, 127, 199, 200});

    // `from_columns` is order-insensitive and tolerates duplicates.
    const std::vector<std::size_t> shuffled{200, 63, 0, 63, 199, 127, 64};
    const Equation same =
        Equation::from_columns(backend, 200, std::span<const std::size_t>(shuffled));
    CHECK(same == equation);
}

TEST_CASE_TEMPLATE("equation: xor_assign", Tag, DenseTag, SparseTag) {
    constexpr Algebra backend = Tag::value;

    Equation left = make_equation_with(backend, 100, {1, 5, 70, 100});
    const Equation right = make_equation_with(backend, 100, {5, 64, 100});
    left.xor_assign(right);
    CHECK(columns_of(left) == std::vector<std::size_t>{1, 64, 70});

    Equation self = make_equation_with(backend, 100, {2, 3, 99});
    const Equation copy = self;
    self.xor_assign(copy);
    CHECK(self.is_eq_true());

    // Addition over GF(2) is its own inverse.
    Equation back = make_equation_with(backend, 100, {1, 64, 70});
    back.xor_assign(right);
    CHECK(columns_of(back) == std::vector<std::size_t>{1, 5, 70, 100});
}

TEST_CASE_TEMPLATE("equation: clear", Tag, DenseTag, SparseTag) {
    constexpr Algebra backend = Tag::value;
    Equation equation = make_equation_with(backend, 70, {0, 40, 70});
    equation.clear();
    CHECK(equation.is_eq_true());
    CHECK(equation.size() == 71); // width unchanged
}

TEST_CASE_TEMPLATE("equation: assign_from refills without allocating", Tag, DenseTag, SparseTag) {
    constexpr Algebra backend = Tag::value;
    constexpr std::size_t num_vars = 500;

    const Equation source = make_equation_with(backend, num_vars, {1, 2, 300, 499, num_vars});
    Equation scratch(backend, num_vars);

    scratch.assign_from(source);
    CHECK(scratch == source);
    CHECK(columns_of(scratch) == columns_of(source));

    // The sparse backend's buffer is now at its high-water mark, so from here on
    // both backends must refill in place. This is the property the whole
    // preallocated-scratch discipline rests on.
    scratch.clear();
    const std::size_t before = allocation_count();
    scratch.assign_from(source);
    const std::size_t after = allocation_count();
    CHECK(after == before);
    CHECK(scratch == source);

    const Equation other = make_equation_with(backend, num_vars, {7});
    scratch.assign_from(other);
    CHECK(scratch == other);
}

#ifdef XORCLE_HAS_DEATH_TESTS
TEST_CASE_TEMPLATE("equation: assign_from rejects a width mismatch", Tag, DenseTag, SparseTag) {
    constexpr Algebra backend = Tag::value;

    // A same-width copy is an internal invariant, so a mismatch asserts rather
    // than silently failing. That can only be observed by watching a child
    // process die, and it is compiled out of release builds along with the
    // assertion itself.
    const pid_t child = ::fork();
    REQUIRE(child >= 0);
    if (child == 0) {
        std::freopen("/dev/null", "w", stderr);
        Equation narrow(backend, 10);
        const Equation wide(backend, 20);
        narrow.assign_from(wide);
        ::_exit(0);
    }
    int status = 0;
    REQUIRE(::waitpid(child, &status, 0) == child);
    CHECK(WIFSIGNALED(status));
}
#endif

TEST_CASE_TEMPLATE("equation: hash agrees with equality", Tag, DenseTag, SparseTag) {
    constexpr Algebra backend = Tag::value;

    const Equation a = make_equation_with(backend, 300, {5, 64, 299, 300});
    const Equation b = make_equation_with(backend, 300, {300, 299, 64, 5});
    const Equation c = make_equation_with(backend, 300, {5, 64, 299});

    CHECK(a == b);
    CHECK(a.hash() == b.hash());
    CHECK_FALSE(a == c);

    const Equation copy = a;
    CHECK(copy == a);
    CHECK(copy.hash() == a.hash());

    std::unordered_map<Equation, int, EquationHash> seen;
    seen[a] = 1;
    seen[c] = 2;
    CHECK(seen.size() == 2);
    CHECK(seen.at(b) == 1);

    CHECK_FALSE(make_equation_with(backend, 10, {1}) == make_equation_with(backend, 11, {1}));
}

TEST_CASE_TEMPLATE("equation: copy and move semantics", Tag, DenseTag, SparseTag) {
    constexpr Algebra backend = Tag::value;

    Equation original = make_equation_with(backend, 50, {1, 2, 3});
    Equation copy = original;
    copy.xor_assign(make_equation_with(backend, 50, {1}));
    CHECK(columns_of(original) == std::vector<std::size_t>{1, 2, 3});
    CHECK(columns_of(copy) == std::vector<std::size_t>{2, 3});

    Equation assigned(backend, 50);
    assigned = original;
    CHECK(assigned == original);

    const Equation moved = std::move(assigned);
    CHECK(moved == original);
}

TEST_CASE_TEMPLATE("equation: project and lift round-trip", Tag, DenseTag, SparseTag) {
    constexpr Algebra backend = Tag::value;
    constexpr std::size_t old_num_vars = 8;

    // Eliminate variables 1, 3 and 6; survivors 0,2,4,5,7 renumber to 0..4.
    Bitmap pivots(old_num_vars + 1);
    pivots.set(1);
    pivots.set(3);
    pivots.set(6);
    const CompactionTransform transform = CompactionTransform::from_pivots(old_num_vars, pivots);
    REQUIRE(transform.new_num_vars() == 5);

    SUBCASE("projection drops eliminated variables and keeps the constant") {
        const Equation old_equation =
            make_equation_with(backend, old_num_vars, {0, 3, 5, old_num_vars});
        const Equation projected = old_equation.project(transform);
        CHECK(projected.num_vars() == 5);
        // 0 -> 0, 3 eliminated, 5 -> 3, constant -> constant.
        CHECK(columns_of(projected) == std::vector<std::size_t>{0, 3, 5});
    }

    SUBCASE("lift undoes projection when nothing was eliminated") {
        const Equation survivors_only =
            make_equation_with(backend, old_num_vars, {0, 2, 7, old_num_vars});
        const Equation projected = survivors_only.project(transform);
        const Equation lifted = projected.lift(transform);
        CHECK(lifted.num_vars() == old_num_vars);
        CHECK(lifted == survivors_only);
    }

    SUBCASE("lift then project is the identity on the compacted space") {
        const Equation compacted = make_equation_with(backend, 5, {0, 4, 5});
        const Equation lifted = compacted.lift(transform);
        CHECK(columns_of(lifted) == std::vector<std::size_t>{0, 7, old_num_vars});
        CHECK(lifted.project(transform) == compacted);
    }

    SUBCASE("the true and false equations survive the round-trip") {
        const Equation eq_false = Equation::eq_false(backend, 5);
        CHECK(eq_false.lift(transform).is_eq_false());
        CHECK(eq_false.lift(transform).project(transform).is_eq_false());
        const Equation eq_true = Equation::eq_true(backend, 5);
        CHECK(eq_true.lift(transform).is_eq_true());
    }
}

TEST_CASE_TEMPLATE("equation: to_string", Tag, DenseTag, SparseTag) {
    constexpr Algebra backend = Tag::value;

    // Columns are zero-based; variables print one-based.
    const Equation equation = make_equation_with(backend, 3, {0, 2, 3});
    const Equation no_constant = make_equation_with(backend, 3, {0, 2});
    const Equation eq_true = Equation::eq_true(backend, 3);
    const Equation eq_false = Equation::eq_false(backend, 3);

    PrintMode mode;
    mode.clause_mode = ClausePrintMode::as_linear_negation;

    CHECK(equation.to_string(mode) == "[x1 + x3 = 1]");
    CHECK(no_constant.to_string(mode) == "[x1 + x3 = 0]");
    CHECK(eq_true.to_string(mode) == "[0 = 0]");
    CHECK(eq_false.to_string(mode) == "[0 = 1]");

    // As a clause literal the constant is negated.
    CHECK(equation.to_string(PrintMode{}) == "[x1 + x3 = 0]");

    mode.equation_mode = EquationPrintMode::equals_0;
    CHECK(equation.to_string(mode) == "[x1 + x3 + 1 = 0]");
    CHECK(no_constant.to_string(mode) == "[x1 + x3 = 0]");
    CHECK(eq_false.to_string(mode) == "[1 = 0]");

    mode.equation_mode = EquationPrintMode::equals_1;
    CHECK(equation.to_string(mode) == "[x1 + x3 = 1]");
    CHECK(no_constant.to_string(mode) == "[x1 + x3 + 1 = 1]");
    CHECK(eq_false.to_string(mode) == "[0 = 1]");
}

TEST_CASE("equation: the two backends agree") {
    constexpr std::size_t num_vars = 137;
    TestRandom random(20260802);

    for (int trial = 0; trial < 200; ++trial) {
        std::vector<std::size_t> columns;
        for (std::size_t column = 0; column <= num_vars; ++column) {
            if (random.chance(4)) {
                columns.push_back(column);
            }
        }
        const std::span<const std::size_t> span(columns);
        Equation dense = Equation::from_columns(Algebra::dense, num_vars, span);
        Equation sparse = Equation::from_columns(Algebra::sparse, num_vars, span);

        CHECK(columns_of(dense) == columns_of(sparse));
        CHECK(dense.to_string() == sparse.to_string());
        CHECK(dense.classify() == sparse.classify());
        CHECK(dense.leading_column() == sparse.leading_column());
        CHECK(dense.is_eq_true() == sparse.is_eq_true());
        CHECK(dense.is_eq_false() == sparse.is_eq_false());

        std::vector<std::size_t> other_columns;
        for (std::size_t column = 0; column <= num_vars; ++column) {
            if (random.chance(3)) {
                other_columns.push_back(column);
            }
        }
        const std::span<const std::size_t> other_span(other_columns);
        dense.xor_assign(Equation::from_columns(Algebra::dense, num_vars, other_span));
        sparse.xor_assign(Equation::from_columns(Algebra::sparse, num_vars, other_span));
        CHECK(columns_of(dense) == columns_of(sparse));
        CHECK(dense.classify() == sparse.classify());
    }
}

} // namespace xorcle
