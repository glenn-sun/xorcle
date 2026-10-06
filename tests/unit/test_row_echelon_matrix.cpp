#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "algebra/equation.h"
#include "algebra/reduce_result.h"
#include "algebra/row_echelon_matrix.h"
#include "algebra_test_support.h"
#include "reference_matrix.h"
#include "util/index_set.h"
#include "util/print_mode.h"

namespace xorcle {

namespace {

void check_matches_reference(const RowEchelonMatrix& matrix, const ReferenceMatrix& reference) {
    std::vector<std::vector<std::size_t>> reference_rows;
    reference_rows.reserve(reference.rows());
    for (std::size_t index = 0; index < reference.rows(); ++index) {
        reference_rows.push_back(set_bits(reference.row(index)));
    }
    REQUIRE(rows_of(matrix) == reference_rows);
    std::vector<std::size_t> pivots;
    std::vector<std::size_t> reference_pivots;
    for (std::size_t col_idx = 0; col_idx < matrix.cols(); ++col_idx) {
        if (matrix.pivot_col_idxs().test(col_idx)) {
            pivots.push_back(col_idx);
        }
        if (reference.is_pivot(col_idx)) {
            reference_pivots.push_back(col_idx);
        }
    }
    REQUIRE(pivots == reference_pivots);
}

} // namespace

TEST_CASE_TEMPLATE("row echelon: empty matrix", Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    const RowEchelonMatrix matrix(repr, 8);

    CHECK(matrix.repr() == repr);
    CHECK(matrix.rows() == 0);
    CHECK(matrix.cols() == 8);
    CHECK(matrix.pivot_col_idxs().size() == 8);
    CHECK(matrix.pivot_col_idxs().none());
    CHECK(matrix.begin() == matrix.end());
    check_echelon_invariant(matrix);

    // A row space over `n` variables reserves the last of its `n + 1` columns
    // for the constant.
    const RowSpace space(repr, 7);
    CHECK(space.num_vars() == 7);
    CHECK(space.cols() == 8);
}

TEST_CASE_TEMPLATE("row echelon: add_row insertion positions", Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    RowSpace matrix(repr, 6);

    // Insert in an order that forces every kind of insertion: at the end, in
    // the middle, and at the front.
    CHECK(matrix.add_row_get_row_idx(make_equation_with(repr, 6, {3, 6})) ==
          std::optional<std::size_t>(0));
    check_echelon_invariant(matrix);

    CHECK(matrix.add_row_get_row_idx(make_equation_with(repr, 6, {5})) ==
          std::optional<std::size_t>(1));
    check_echelon_invariant(matrix);

    CHECK(matrix.add_row_get_row_idx(make_equation_with(repr, 6, {1, 2})) ==
          std::optional<std::size_t>(0));
    check_echelon_invariant(matrix);

    CHECK(matrix.add_row_get_row_idx(make_equation_with(repr, 6, {4, 6})) ==
          std::optional<std::size_t>(2));
    check_echelon_invariant(matrix);

    CHECK(matrix.rows() == 4);
    CHECK(bits_of(matrix.row(0)) == std::vector<std::size_t>{1, 2});
    CHECK(bits_of(matrix.row(1)) == std::vector<std::size_t>{3, 6});
    CHECK(bits_of(matrix.row(2)) == std::vector<std::size_t>{4, 6});
    CHECK(bits_of(matrix.row(3)) == std::vector<std::size_t>{5});
}

TEST_CASE_TEMPLATE("row echelon: add_row is idempotent on in-span rows", Tag, DenseTag,
                   SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    RowSpace matrix(repr, 5);

    const Equation a = make_equation_with(repr, 5, {0, 2});
    const Equation b = make_equation_with(repr, 5, {1, 5});
    REQUIRE(matrix.add_row_get_row_idx(a).has_value());
    REQUIRE(matrix.add_row_get_row_idx(b).has_value());
    REQUIRE(matrix.rows() == 2);

    // The row itself, a sum of stored rows, and the true equation are all in
    // the span and must be rejected without changing anything.
    CHECK(matrix.add_row_get_row_idx(a) == std::nullopt);
    Equation sum = a;
    sum.plus_assign(b);
    CHECK(matrix.add_row_get_row_idx(sum) == std::nullopt);
    CHECK(matrix.add_row_get_row_idx(Equation::eq_true(repr, 5)) == std::nullopt);
    CHECK(matrix.rows() == 2);

    matrix.add_row(a);
    CHECK(matrix.rows() == 2);
    check_echelon_invariant(matrix);

    // The false equation is not in this span, and its leading column is the
    // constant column.
    CHECK(matrix.add_row_get_row_idx(Equation::eq_false(repr, 5)) ==
          std::optional<std::size_t>(2));
    CHECK(matrix.pivot_col_idxs().test(5));
    check_echelon_invariant(matrix);
}

TEST_CASE_TEMPLATE("row echelon: reduce and partial_reduce", Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    RowSpace matrix(repr, 4);
    matrix.add_row(make_equation_with(repr, 4, {0, 2}));
    matrix.add_row(make_equation_with(repr, 4, {1, 4}));

    SUBCASE("a row in the span reduces to the true equation") {
        Equation v = make_equation_with(repr, 4, {0, 1, 2, 4});
        CHECK(matrix.partial_reduce(v) == ReduceResult::eq_true());
        CHECK(v.is_eq_true());
    }

    SUBCASE("partial_reduce stops at the first non-pivot column") {
        Equation v = make_equation_with(repr, 4, {0, 3});
        CHECK(matrix.partial_reduce(v) == ReduceResult::leading(2));
        // Only the leading pivot was eliminated; column 3 is untouched.
        CHECK(bits_of(v) == std::vector<std::size_t>{2, 3});
    }

    SUBCASE("reduce eliminates every pivot column") {
        Equation v = make_equation_with(repr, 4, {0, 1, 3});
        matrix.reduce(v);
        CHECK(bits_of(v) == std::vector<std::size_t>{2, 3, 4});
        CHECK(matrix.pivot_col_idxs().test(0) == true);
        for (const std::size_t col_idx : bits_of(v)) {
            CHECK_FALSE(matrix.pivot_col_idxs().test(col_idx));
        }
    }

    SUBCASE("a reduction to the false equation is reported as eq_false") {
        RowSpace contradictory(repr, 4);
        contradictory.add_row(make_equation_with(repr, 4, {0, 4}));
        Equation v = make_equation_with(repr, 4, {0});
        CHECK(contradictory.partial_reduce(v) == ReduceResult::eq_false());
        CHECK(v.is_eq_false());
    }
}

TEST_CASE_TEMPLATE("row echelon: partial_reduce_below skips the rows the bound excludes", Tag,
                   DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    RowSpace matrix(repr, 4);
    matrix.add_row(make_equation_with(repr, 4, {0, 2}));
    matrix.add_row(make_equation_with(repr, 4, {1, 3}));
    // The row pivoting column 1 comes first in the order, the one pivoting
    // column 0 second.
    const std::vector<std::size_t> row_idx_to_order{1, 0};

    SUBCASE("a row below the bound reduces") {
        Equation v = make_equation_with(repr, 4, {1, 2});
        CHECK(matrix.partial_reduce_below(v, row_idx_to_order, 1) == ReduceResult::leading(2));
        CHECK(bits_of(v) == std::vector<std::size_t>{2, 3});
    }

    SUBCASE("a row at the bound stops the reduction on its own pivot") {
        Equation v = make_equation_with(repr, 4, {0, 3});
        CHECK(matrix.partial_reduce_below(v, row_idx_to_order, 1) == ReduceResult::leading(0));
        CHECK(bits_of(v) == std::vector<std::size_t>{0, 3});
    }

    SUBCASE("a bound of zero reduces by nothing") {
        Equation v = make_equation_with(repr, 4, {0, 1});
        CHECK(matrix.partial_reduce_below(v, row_idx_to_order, 0) == ReduceResult::leading(0));
        CHECK(bits_of(v) == std::vector<std::size_t>{0, 1});
    }

    SUBCASE("a bound above every order behaves as partial_reduce") {
        Equation v = make_equation_with(repr, 4, {0, 1});
        Equation reference = make_equation_with(repr, 4, {0, 1});
        const ReduceResult result = matrix.partial_reduce_below(v, row_idx_to_order, 2);
        CHECK(result == matrix.partial_reduce(reference));
        CHECK(bits_of(v) == bits_of(reference));
    }

    SUBCASE("a row in the span of the rows below the bound reduces to the true equation") {
        Equation v = make_equation_with(repr, 4, {1, 3});
        CHECK(matrix.partial_reduce_below(v, row_idx_to_order, 1) == ReduceResult::eq_true());
        CHECK(v.is_eq_true());
    }
}

TEST_CASE_TEMPLATE("row echelon: partial_reduce_below matches a matrix of the rows below the bound",
                   Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    constexpr std::size_t num_vars = 20;
    TestRandom random(24680);

    for (int trial = 0; trial < 300; ++trial) {
        RowSpace matrix(repr, num_vars);
        const std::size_t basis_size = random.next(num_vars);
        for (std::size_t i = 0; i < basis_size; ++i) {
            matrix.add_row(to_equation(repr, random_bits(random, num_vars, 3)));
        }

        std::vector<std::size_t> row_idx_to_order(matrix.rows());
        for (std::size_t row_idx = 0; row_idx < row_idx_to_order.size(); ++row_idx) {
            row_idx_to_order[row_idx] = row_idx;
        }
        for (std::size_t i = row_idx_to_order.size(); i-- > 1;) {
            std::swap(row_idx_to_order[i], row_idx_to_order[random.next(i + 1)]);
        }
        const std::size_t bound = random.next(matrix.rows() + 1);

        RowSpace below(repr, num_vars);
        for (std::size_t row_idx = 0; row_idx < matrix.rows(); ++row_idx) {
            if (row_idx_to_order[row_idx] < bound) {
                below.add_row(matrix.row(row_idx));
            }
        }

        const Bits probe = random_bits(random, num_vars, 3);
        Equation restricted = to_equation(repr, probe);
        Equation reference = to_equation(repr, probe);
        const ReduceResult result = matrix.partial_reduce_below(restricted, row_idx_to_order, bound);
        CHECK(result == below.partial_reduce(reference));
        CHECK(bits_of(restricted) == bits_of(reference));
    }
}

TEST_CASE_TEMPLATE("row echelon: reduce records its coefficients", Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    RowSpace matrix(repr, 5);
    matrix.add_row(make_equation_with(repr, 5, {0, 3}));
    matrix.add_row(make_equation_with(repr, 5, {1, 4, 5}));
    matrix.add_row(make_equation_with(repr, 5, {2, 5}));
    REQUIRE(matrix.rows() == 3);

    Equation v = make_equation_with(repr, 5, {0, 2, 4});
    const Equation original = v;

    const IndexSet coefficients = matrix.reduce_get_coefficients(v);
    CHECK(coefficients.size() == matrix.rows());

    // The recorded rows must sum to exactly the change reduction made.
    Equation rebuilt = original;
    coefficients.for_each_set([&](std::size_t index) { rebuilt.plus_assign(matrix.row(index)); });
    CHECK(rebuilt == v);
    CHECK(coefficients.count() > 0);
}

TEST_CASE_TEMPLATE("row echelon: reduce writes its coefficients into a caller's set", Tag,
                   DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    RowSpace matrix(repr, 5);
    matrix.add_row(make_equation_with(repr, 5, {0, 3}));
    matrix.add_row(make_equation_with(repr, 5, {1, 4, 5}));
    matrix.add_row(make_equation_with(repr, 5, {2, 5}));
    REQUIRE(matrix.rows() == 3);

    // A set arriving with a stale size and stale indices, as a reused one does.
    IndexSet coefficients(7);
    coefficients.set(0);
    coefficients.set(6);

    Equation v = make_equation_with(repr, 5, {0, 2, 4});
    Equation by_value = v;
    const IndexSet expected = matrix.reduce_get_coefficients(by_value);

    matrix.reduce_get_coefficients(v, coefficients);
    CHECK(coefficients.size() == matrix.rows());
    CHECK(coefficients == expected);
    CHECK(v == by_value);

    SUBCASE("a reduction using no row leaves the set empty") {
        Equation independent = make_equation_with(repr, 5, {3});
        matrix.reduce_get_coefficients(independent, coefficients);
        CHECK(coefficients.size() == matrix.rows());
        CHECK(coefficients.none());
    }
}

TEST_CASE_TEMPLATE("row echelon: partial_reduce_with_extra matches an augmented matrix", Tag,
                   DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    constexpr std::size_t num_vars = 12;
    TestRandom random(4242);

    for (int trial = 0; trial < 100; ++trial) {
        RowSpace matrix(repr, num_vars);
        for (int i = 0; i < 4; ++i) {
            matrix.add_row(to_equation(repr, random_bits(random, num_vars, 3)));
        }

        // `extra` must be partially reduced by the matrix and non-zero, which is
        // exactly the precondition `partial_reduce_with_extra()` states.
        Equation extra = to_equation(repr, random_bits(random, num_vars, 3));
        if (matrix.partial_reduce(extra) == ReduceResult::eq_true()) {
            continue;
        }

        RowSpace augmented = matrix;
        REQUIRE(augmented.add_row_get_row_idx(extra).has_value());
        check_echelon_invariant(augmented);

        const Equation v = to_equation(repr, random_bits(random, num_vars, 3));

        Equation partial_with_extra = v;
        Equation partial_via_augmented = v;
        const ReduceResult a = matrix.partial_reduce_with_extra(partial_with_extra, extra);
        const ReduceResult b = augmented.partial_reduce(partial_via_augmented);
        CHECK(a == b);
        CHECK(partial_with_extra == partial_via_augmented);

        if (v.leading_bit()) {
            Equation partial_from_leading = v;
            CHECK(matrix.partial_reduce_with_extra(partial_from_leading, *v.leading_bit(), extra,
                                                   extra.leading_bit()) == a);
            CHECK(partial_from_leading == partial_with_extra);
        }
    }
}

TEST_CASE_TEMPLATE("row echelon: partial_reduce_with_extra tolerates a zero extra", Tag, DenseTag,
                   SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    RowSpace matrix(repr, 4);
    matrix.add_row(make_equation_with(repr, 4, {0, 2}));

    const Equation zero_extra = Equation::eq_true(repr, 4);
    Equation v = make_equation_with(repr, 4, {0, 3});
    Equation without = v;
    CHECK(matrix.partial_reduce_with_extra(v, zero_extra) == matrix.partial_reduce(without));
    CHECK(v == without);
}

TEST_CASE_TEMPLATE("row echelon: pop_first_row and pop_last_row", Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    RowSpace matrix(repr, 6);
    matrix.add_row(make_equation_with(repr, 6, {0, 6}));
    matrix.add_row(make_equation_with(repr, 6, {2}));
    matrix.add_row(make_equation_with(repr, 6, {4, 5}));
    REQUIRE(matrix.rows() == 3);

    const Equation first = matrix.pop_first_row();
    CHECK(bits_of(first) == std::vector<std::size_t>{0, 6});
    CHECK(matrix.rows() == 2);
    CHECK_FALSE(matrix.pivot_col_idxs().test(0));
    CHECK(bits_of(matrix.row(0)) == std::vector<std::size_t>{2});
    CHECK(bits_of(matrix.row(1)) == std::vector<std::size_t>{4, 5});
    check_echelon_invariant(matrix);

    matrix.pop_last_row();
    CHECK(matrix.rows() == 1);
    CHECK_FALSE(matrix.pivot_col_idxs().test(4));
    CHECK(bits_of(matrix.row(0)) == std::vector<std::size_t>{2});
    check_echelon_invariant(matrix);

    matrix.pop_last_row();
    CHECK(matrix.rows() == 0);
    CHECK(matrix.pivot_col_idxs().none());
    check_echelon_invariant(matrix);
}

namespace {

/// @brief Drive a matrix and `ReferenceMatrix` through the same random
/// insertions, row additions, reductions and pops, checking every result.
void exercise_against_reference(EquationRepr repr, std::size_t num_vars, int trials, int steps,
                                std::uint64_t seed) {
    TestRandom random(seed);
    for (int trial = 0; trial < trials; ++trial) {
        RowSpace matrix(repr, num_vars);
        ReferenceMatrix reference(num_vars);

        for (int step = 0; step < steps; ++step) {
            const Bits candidate = random_bits(random, num_vars, 4);
            const std::optional<std::size_t> actual =
                matrix.add_row_get_row_idx(to_equation(repr, candidate));
            const std::optional<std::size_t> expected = reference.add_row_get_row_idx(candidate);
            REQUIRE(actual == expected);
            check_matches_reference(matrix, reference);
            check_echelon_invariant(matrix);

            const Bits probe = random_bits(random, num_vars, 4);
            Equation reduced = to_equation(repr, probe);
            Bits reference_reduced = probe;
            matrix.reduce(reduced);
            reference.reduce(reference_reduced);
            CHECK(to_bits(reduced) == reference_reduced);

            Equation partial = to_equation(repr, probe);
            Bits reference_partial = probe;
            const ReduceResult result = matrix.partial_reduce(partial);
            const std::optional<std::size_t> reference_leading =
                reference.partial_reduce(reference_partial);
            CHECK(result == expected_result(reference_leading, num_vars));
            CHECK(to_bits(partial) == reference_partial);

            Equation partial_from_leading = to_equation(repr, probe);
            if (const std::optional<std::size_t> leading = partial_from_leading.leading_bit()) {
                CHECK(matrix.partial_reduce(partial_from_leading, *leading) == result);
                CHECK(partial_from_leading == partial);
            }

            std::vector<std::size_t> carried;
            matrix.carried_col_idxs().for_each_set(
                [&carried](std::size_t col_idx) { carried.push_back(col_idx); });
            CHECK(carried == reference.carried_col_idxs());
            for (std::size_t col_idx = 0; col_idx <= num_vars; ++col_idx) {
                CHECK(matrix.carries(col_idx) ==
                      std::ranges::binary_search(carried, col_idx));
            }

            if (matrix.rows() > 0 && random.chance(3)) {
                const std::size_t row_idx = random.next(matrix.rows());
                Bits addend = random_bits(random, num_vars, 3);
                const std::size_t lead = *matrix.row(row_idx).leading_bit();
                for (std::size_t bit = 0; bit <= lead; ++bit) {
                    addend[bit] = false;
                }
                if (!set_bits(addend).empty()) {
                    matrix.add_to_row(row_idx, to_equation(repr, addend));
                    reference.add_to_row(row_idx, addend);
                    check_matches_reference(matrix, reference);
                    check_echelon_invariant(matrix);
                }
            }
            if (random.chance(8)) {
                matrix.reduce_rows();
                reference.reduce_rows();
                check_matches_reference(matrix, reference);
                check_echelon_invariant(matrix);
            }
        }

        while (matrix.rows() > 0) {
            switch (random.next(3)) {
                case 0:
                    CHECK(to_bits(matrix.pop_first_row()) == reference.pop_first_row());
                    break;
                case 1:
                    matrix.pop_last_row();
                    reference.pop_last_row();
                    break;
                default: {
                    const std::size_t row_idx = random.next(matrix.rows());
                    CHECK(to_bits(matrix.pop_row(row_idx)) == reference.pop_row(row_idx));
                }
            }
            check_matches_reference(matrix, reference);
            check_echelon_invariant(matrix);
        }
    }
}

} // namespace

TEST_CASE_TEMPLATE("row echelon: matches the reference matrix", Tag, DenseTag, SparseTag) {
    exercise_against_reference(Tag::value, 17, 60, 40, 987654321);
}

// Two and three words a row, where the dense word loops and the pivot tables
// meet their word boundaries.
TEST_CASE_TEMPLATE("row echelon: matches the reference matrix past one word", Tag, DenseTag,
                   SparseTag) {
    exercise_against_reference(Tag::value, 70, 2, 50, 1234567);
    exercise_against_reference(Tag::value, 130, 1, 70, 7654321);
}

TEST_CASE_TEMPLATE("row echelon: a reduced row never leads on a pivot column", Tag, DenseTag,
                   SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    constexpr std::size_t num_vars = 24;
    TestRandom random(31337);

    for (int trial = 0; trial < 300; ++trial) {
        RowSpace matrix(repr, num_vars);
        const std::size_t basis_size = random.next(num_vars);
        for (std::size_t i = 0; i < basis_size; ++i) {
            matrix.add_row(to_equation(repr, random_bits(random, num_vars, 3)));
        }
        check_echelon_invariant(matrix);

        Equation v = to_equation(repr, random_bits(random, num_vars, 3));
        const Equation original = v;
        const ReduceResult result = matrix.partial_reduce(v);

        switch (result.kind) {
            case ReduceResult::Kind::eq_true:
                CHECK(v.is_eq_true());
                break;
            case ReduceResult::Kind::eq_false:
                CHECK(v.is_eq_false());
                CHECK(matrix.pivot_col_idxs().test(num_vars) == false);
                break;
            case ReduceResult::Kind::leading:
                CHECK(v.leading_bit() == std::optional<std::size_t>(result.bit));
                CHECK_FALSE(matrix.pivot_col_idxs().test(result.bit));
                break;
        }

        Equation fully = original;
        matrix.reduce(fully);
        for (const std::size_t col_idx : bits_of(fully)) {
            CHECK_FALSE(matrix.pivot_col_idxs().test(col_idx));
        }
        CHECK(fully.classify() == result);

        RowSpace extended = matrix;
        CHECK(extended.add_row_get_row_idx(original).has_value() ==
              (result.kind != ReduceResult::Kind::eq_true));
    }
}

TEST_CASE_TEMPLATE("row space: unique_solution", Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    constexpr std::size_t num_vars = 20;
    TestRandom random(11111);

    for (int trial = 0; trial < 40; ++trial) {
        // Pick a target assignment, then feed in random equations that it
        // satisfies until the system is full-rank.
        std::vector<bool> target(num_vars);
        for (std::size_t i = 0; i < num_vars; ++i) {
            target[i] = random.chance(2);
        }

        RowSpace space(repr, num_vars);
        std::size_t guard = 0;
        while (space.rows() < num_vars && guard++ < 10000) {
            Bits row(num_vars + 1, false);
            bool constant = false;
            for (std::size_t i = 0; i < num_vars; ++i) {
                if (random.chance(3)) {
                    row[i] = true;
                    constant = constant != target[i];
                }
            }
            row[num_vars] = constant;
            space.add_row(to_equation(repr, row));
        }
        REQUIRE(space.rows() == num_vars);
        check_echelon_invariant(space);
        REQUIRE_FALSE(space.pivot_col_idxs().test(num_vars));

        const IndexSet solution = space.unique_solution();
        REQUIRE(solution.size() == num_vars);
        for (std::size_t i = 0; i < num_vars; ++i) {
            CHECK(solution.test(i) == target[i]);
        }
    }
}

TEST_CASE_TEMPLATE("clause: to_string", Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;

    // The clause [x1 + x2 = 1] v [x3 = 0] is stored as its linear negation,
    // [x1 + x2 = 0] ^ [x3 = 1].
    Clause clause(repr, 3);
    clause.add_row(make_equation_with(repr, 3, {0, 1}));
    clause.add_row(make_equation_with(repr, 3, {2, 3}));
    REQUIRE(clause.rows() == 2);

    CHECK(clause.to_string() == "[x1 + x2 = 1] v [x3 = 0]");

    PrintMode as_stored;
    as_stored.clause_mode = ClausePrintMode::as_linear_negation;
    CHECK(clause.to_string(as_stored) == "[x1 + x2 = 0] ^ [x3 = 1]");

    clause.watched0 = 1;
    clause.next_watch_search_start = 2;
    const Clause copy = clause;
    CHECK(copy.watched0 == 1);
    CHECK(copy.watched1 == 1); // default, untouched
    CHECK(copy.next_watch_search_start == 2);
    CHECK(copy.to_string() == clause.to_string());
    check_echelon_invariant(copy);
}

TEST_CASE_TEMPLATE("row echelon: copy is deep", Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    RowSpace original(repr, 5);
    original.add_row(make_equation_with(repr, 5, {0, 5}));

    RowSpace copy = original;
    copy.add_row(make_equation_with(repr, 5, {2}));
    CHECK(original.rows() == 1);
    CHECK(copy.rows() == 2);

    RowSpace assigned(repr, 5);
    assigned = copy;
    CHECK(assigned.rows() == 2);
    assigned.pop_last_row();
    CHECK(copy.rows() == 2);
    check_echelon_invariant(original);
    check_echelon_invariant(copy);
    check_echelon_invariant(assigned);
}

// Zassenhaus hands back a basis of the intersection: every row lies in both
// spans, the rows are independent, and there are as many as
// dim U + dim W - dim (U + W).
TEST_CASE_TEMPLATE("row echelon: span_intersection is a basis of the intersection", Tag,
                   DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    TestRandom random(271828);
    for (const std::size_t num_vars : {std::size_t{9}, std::size_t{70}}) {
        for (int trial = 0; trial < 25; ++trial) {
            RowSpace left(repr, num_vars);
            RowSpace right(repr, num_vars);
            RowSpace sum(repr, num_vars);
            // Some shared rows, so the intersection is often more than zero.
            for (std::size_t count = random.next(num_vars / 2 + 2); count > 0; --count) {
                const Equation row = to_equation(repr, random_bits(random, num_vars, 3));
                left.add_row(row);
                right.add_row(row);
            }
            for (RowSpace* space : {&left, &right}) {
                for (std::size_t count = random.next(num_vars / 2 + 2); count > 0; --count) {
                    space->add_row(to_equation(repr, random_bits(random, num_vars, 3)));
                }
            }
            for (const RowSpace* space : {&left, &right}) {
                for (const Equation& row : *space) {
                    sum.add_row(row);
                }
            }

            const std::vector<Equation> basis = span_intersection(left, right);
            RowSpace found(repr, num_vars);
            for (const Equation& row : basis) {
                for (const RowSpace* space : {&left, &right}) {
                    Equation reduced = row;
                    space->reduce(reduced);
                    CHECK(reduced.is_eq_true());
                }
                CHECK(found.add_row_get_row_idx(row).has_value());
            }
            CHECK(basis.size() == left.rows() + right.rows() - sum.rows());
        }
    }
}

} // namespace xorcle
