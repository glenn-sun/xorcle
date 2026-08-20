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
#include "util/index_set.h"
#include "util/print_mode.h"

#if !defined(NDEBUG) && (defined(__APPLE__) || defined(__unix__))
#define XORCLE_HAS_DEATH_TESTS 1
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace xorcle {

TEST_CASE_TEMPLATE("equation: construction and bit count", Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;

    Equation null_handle;
    CHECK(null_handle.is_null());
    CHECK(null_handle.num_bits() == 0);

    Equation equation(repr, 5);
    CHECK_FALSE(equation.is_null());
    CHECK(equation.repr() == repr);
    CHECK(equation.num_vars() == 5);
    CHECK(equation.num_bits() == 6);
    CHECK(equation.is_eq_true());
    CHECK(bits_of(equation).empty());

    equation.resize(9);
    CHECK(equation.num_vars() == 9);
    CHECK(equation.num_bits() == 10);
    CHECK(equation.is_eq_true());
}

TEST_CASE_TEMPLATE("equation: eq_true, eq_false and classify", Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    constexpr std::size_t num_vars = 130; // spans three words in the dense representation

    const Equation eq_true = Equation::eq_true(repr, num_vars);
    CHECK(eq_true.classify() == ReduceResult::eq_true());
    CHECK(eq_true.is_eq_true());
    CHECK_FALSE(eq_true.is_eq_false());
    CHECK(eq_true.leading_bit() == std::nullopt);

    const Equation eq_false = Equation::eq_false(repr, num_vars);
    CHECK(eq_false.classify() == ReduceResult::eq_false());
    CHECK(eq_false.is_eq_false());
    CHECK_FALSE(eq_false.is_eq_true());
    CHECK(bits_of(eq_false) == std::vector<std::size_t>{num_vars});
    CHECK(eq_false.leading_bit() == std::optional<std::size_t>(num_vars));

    SUBCASE("classify covers every shape of the final word") {
        // A three-way classification composed from separate emptiness and
        // constant-bit queries is easy to get wrong for equations whose final
        // word holds a variable bit. Each shape below is one such case.

        // Only a variable bit set, in the final block.
        const Equation only_variable = make_equation_with(repr, num_vars, {129});
        CHECK(only_variable.classify() == ReduceResult::leading(129));
        CHECK(only_variable.classify().is_leading());
        CHECK_FALSE(only_variable.is_eq_true());
        CHECK_FALSE(only_variable.is_eq_false());

        // A variable bit and the constant bit together.
        const Equation with_constant = make_equation_with(repr, num_vars, {129, num_vars});
        CHECK(with_constant.classify() == ReduceResult::leading(129));
        CHECK_FALSE(with_constant.is_eq_true());
        CHECK_FALSE(with_constant.is_eq_false());

        // A variable bit in an earlier block, so the leading bit is not in the
        // final block at all.
        const Equation early = make_equation_with(repr, num_vars, {3, 129, num_vars});
        CHECK(early.classify() == ReduceResult::leading(3));
    }
}

TEST_CASE_TEMPLATE("equation: bits and leading_bit", Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    const Equation equation = make_equation_with(repr, 200, {0, 63, 64, 127, 199, 200});

    CHECK(bits_of(equation) == std::vector<std::size_t>{0, 63, 64, 127, 199, 200});
    CHECK(equation.leading_bit() == std::optional<std::size_t>(0));

    // A null handle has no bits at all.
    CHECK(Equation().bits().empty());

    // `from_bits` is order-insensitive and tolerates duplicates.
    const std::vector<std::size_t> shuffled{200, 63, 0, 63, 199, 127, 64};
    const Equation same =
        Equation::from_bits(repr, 200, std::span<const std::size_t>(shuffled));
    CHECK(same == equation);
}

TEST_CASE_TEMPLATE("equation: plus_assign", Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;

    Equation left = make_equation_with(repr, 100, {1, 5, 70, 100});
    const Equation right = make_equation_with(repr, 100, {5, 64, 100});
    left.plus_assign(right);
    CHECK(bits_of(left) == std::vector<std::size_t>{1, 64, 70});

    Equation self = make_equation_with(repr, 100, {2, 3, 99});
    const Equation copy = self;
    self.plus_assign(copy);
    CHECK(self.is_eq_true());

    // Addition over GF(2) is its own inverse.
    Equation back = make_equation_with(repr, 100, {1, 64, 70});
    back.plus_assign(right);
    CHECK(bits_of(back) == std::vector<std::size_t>{1, 5, 70, 100});
}

TEST_CASE_TEMPLATE("equation: set_eq_true", Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    Equation equation = make_equation_with(repr, 70, {0, 40, 70});
    equation.set_eq_true();
    CHECK(equation.is_eq_true());
    CHECK(equation.num_bits() == 71); // bit count unchanged
}

TEST_CASE_TEMPLATE("equation: assign_from refills without allocating", Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    constexpr std::size_t num_vars = 500;

    const Equation source = make_equation_with(repr, num_vars, {1, 2, 300, 499, num_vars});
    Equation scratch(repr, num_vars);

    scratch.assign_from(source);
    CHECK(scratch == source);
    CHECK(bits_of(scratch) == bits_of(source));

    // The sparse buffer is now at its high-water mark, so from here on both
    // representations must refill in place. This is the property the whole
    // preallocated-scratch discipline rests on.
    scratch.set_eq_true();
    const std::size_t before = allocation_count();
    scratch.assign_from(source);
    const std::size_t after = allocation_count();
    CHECK(after == before);
    CHECK(scratch == source);

    const Equation other = make_equation_with(repr, num_vars, {7});
    scratch.assign_from(other);
    CHECK(scratch == other);
}

#ifdef XORCLE_HAS_DEATH_TESTS
TEST_CASE_TEMPLATE("equation: assign_from rejects a bit-count mismatch", Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;

    // A same-bit-count copy is an internal invariant, so a mismatch asserts rather
    // than silently failing. That can only be observed by watching a child
    // process die, and it is compiled out of release builds along with the
    // assertion itself.
    const pid_t child = ::fork();
    REQUIRE(child >= 0);
    if (child == 0) {
        std::freopen("/dev/null", "w", stderr);
        Equation narrow(repr, 10);
        const Equation wide(repr, 20);
        narrow.assign_from(wide);
        ::_exit(0);
    }
    int status = 0;
    REQUIRE(::waitpid(child, &status, 0) == child);
    CHECK(WIFSIGNALED(status));
}
#endif

TEST_CASE_TEMPLATE("equation: hash agrees with equality", Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;

    const Equation a = make_equation_with(repr, 300, {5, 64, 299, 300});
    const Equation b = make_equation_with(repr, 300, {300, 299, 64, 5});
    const Equation c = make_equation_with(repr, 300, {5, 64, 299});

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

    CHECK_FALSE(make_equation_with(repr, 10, {1}) == make_equation_with(repr, 11, {1}));
}

TEST_CASE_TEMPLATE("equation: copy and move semantics", Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;

    Equation original = make_equation_with(repr, 50, {1, 2, 3});
    Equation copy = original;
    copy.plus_assign(make_equation_with(repr, 50, {1}));
    CHECK(bits_of(original) == std::vector<std::size_t>{1, 2, 3});
    CHECK(bits_of(copy) == std::vector<std::size_t>{2, 3});

    Equation assigned(repr, 50);
    assigned = original;
    CHECK(assigned == original);

    const Equation moved = std::move(assigned);
    CHECK(moved == original);
}

TEST_CASE_TEMPLATE("equation: compact and uncompact round-trip", Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    constexpr std::size_t old_num_vars = 8;

    // Eliminate variables 1, 3 and 6; survivors 0,2,4,5,7 renumber to 0..4.
    IndexSet dropped(old_num_vars + 1);
    dropped.set(1);
    dropped.set(3);
    dropped.set(6);
    const CompactionTransform transform =
        CompactionTransform::from_dropped_columns(old_num_vars, dropped);
    REQUIRE(transform.new_num_vars() == 5);

    SUBCASE("compaction drops eliminated variables and keeps the constant") {
        const Equation old_equation =
            make_equation_with(repr, old_num_vars, {0, 3, 5, old_num_vars});
        const Equation compacted = old_equation.compact(transform);
        CHECK(compacted.num_vars() == 5);
        // 0 -> 0, 3 eliminated, 5 -> 3, constant -> constant.
        CHECK(bits_of(compacted) == std::vector<std::size_t>{0, 3, 5});
    }

    SUBCASE("uncompact undoes compaction when nothing was eliminated") {
        const Equation survivors_only =
            make_equation_with(repr, old_num_vars, {0, 2, 7, old_num_vars});
        const Equation compacted = survivors_only.compact(transform);
        const Equation restored = compacted.uncompact(transform);
        CHECK(restored.num_vars() == old_num_vars);
        CHECK(restored == survivors_only);
    }

    SUBCASE("uncompact then compact is the identity on the compacted space") {
        const Equation compacted = make_equation_with(repr, 5, {0, 4, 5});
        const Equation restored = compacted.uncompact(transform);
        CHECK(bits_of(restored) == std::vector<std::size_t>{0, 7, old_num_vars});
        CHECK(restored.compact(transform) == compacted);
    }

    SUBCASE("the true and false equations survive the round-trip") {
        const Equation eq_false = Equation::eq_false(repr, 5);
        CHECK(eq_false.uncompact(transform).is_eq_false());
        CHECK(eq_false.uncompact(transform).compact(transform).is_eq_false());
        const Equation eq_true = Equation::eq_true(repr, 5);
        CHECK(eq_true.uncompact(transform).is_eq_true());
    }
}

TEST_CASE_TEMPLATE("equation: to_string", Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;

    // Bits are zero-based; variables print one-based.
    const Equation equation = make_equation_with(repr, 3, {0, 2, 3});
    const Equation no_constant = make_equation_with(repr, 3, {0, 2});
    const Equation eq_true = Equation::eq_true(repr, 3);
    const Equation eq_false = Equation::eq_false(repr, 3);

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

TEST_CASE("equation: the two representations agree") {
    constexpr std::size_t num_vars = 137;
    TestRandom random(20260802);

    for (int trial = 0; trial < 200; ++trial) {
        std::vector<std::size_t> bits;
        for (std::size_t bit = 0; bit <= num_vars; ++bit) {
            if (random.chance(4)) {
                bits.push_back(bit);
            }
        }
        const std::span<const std::size_t> span(bits);
        Equation dense = Equation::from_bits(EquationRepr::dense, num_vars, span);
        Equation sparse = Equation::from_bits(EquationRepr::sparse, num_vars, span);

        CHECK(bits_of(dense) == bits_of(sparse));
        CHECK(dense.to_string() == sparse.to_string());
        CHECK(dense.classify() == sparse.classify());
        CHECK(dense.leading_bit() == sparse.leading_bit());
        CHECK(dense.is_eq_true() == sparse.is_eq_true());
        CHECK(dense.is_eq_false() == sparse.is_eq_false());

        std::vector<std::size_t> other_bits;
        for (std::size_t bit = 0; bit <= num_vars; ++bit) {
            if (random.chance(3)) {
                other_bits.push_back(bit);
            }
        }
        const std::span<const std::size_t> other_span(other_bits);
        dense.plus_assign(Equation::from_bits(EquationRepr::dense, num_vars, other_span));
        sparse.plus_assign(Equation::from_bits(EquationRepr::sparse, num_vars, other_span));
        CHECK(bits_of(dense) == bits_of(sparse));
        CHECK(dense.classify() == sparse.classify());
    }
}

} // namespace xorcle
