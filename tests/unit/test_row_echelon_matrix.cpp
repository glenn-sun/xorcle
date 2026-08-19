#include <doctest/doctest.h>

#include <cstddef>
#include <optional>
#include <vector>

#include "algebra/equation.h"
#include "algebra/reduce_result.h"
#include "algebra/row_echelon_matrix.h"
#include "algebra_test_support.h"
#include "reference_matrix.h"
#include "util/bitmap.h"
#include "util/print_mode.h"

namespace xorcle {

namespace {

void check_matches_reference(const RowEchelonMatrix& matrix, const ReferenceMatrix& reference) {
    REQUIRE(matrix.rows() == reference.rows());
    for (std::size_t index = 0; index < matrix.rows(); ++index) {
        CHECK(bits_of(matrix.row(index)) == set_bits(reference.row(index)));
    }
    for (std::size_t column = 0; column < matrix.cols(); ++column) {
        REQUIRE(matrix.pivot_columns().test(column) == reference.is_pivot(column));
    }
}

} // namespace

TEST_CASE_TEMPLATE("row echelon: empty matrix", Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    const RowEchelonMatrix matrix(repr, 7);

    CHECK(matrix.repr() == repr);
    CHECK(matrix.rows() == 0);
    CHECK(matrix.num_vars() == 7);
    CHECK(matrix.cols() == 8);
    CHECK(matrix.pivot_columns().size() == 8);
    CHECK(matrix.pivot_columns().none());
    CHECK(matrix.begin() == matrix.end());
    check_echelon_invariant(matrix);
}

TEST_CASE_TEMPLATE("row echelon: add_row insertion positions", Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    RowEchelonMatrix matrix(repr, 6);

    // Insert in an order that forces every kind of insertion: at the end, in
    // the middle, and at the front.
    CHECK(matrix.add_row_get_index(make_equation_with(repr, 6, {3, 6})) ==
          std::optional<std::size_t>(0));
    check_echelon_invariant(matrix);

    CHECK(matrix.add_row_get_index(make_equation_with(repr, 6, {5})) ==
          std::optional<std::size_t>(1));
    check_echelon_invariant(matrix);

    CHECK(matrix.add_row_get_index(make_equation_with(repr, 6, {1, 2})) ==
          std::optional<std::size_t>(0));
    check_echelon_invariant(matrix);

    CHECK(matrix.add_row_get_index(make_equation_with(repr, 6, {4, 6})) ==
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
    RowEchelonMatrix matrix(repr, 5);

    const Equation a = make_equation_with(repr, 5, {0, 2});
    const Equation b = make_equation_with(repr, 5, {1, 5});
    REQUIRE(matrix.add_row_get_index(a).has_value());
    REQUIRE(matrix.add_row_get_index(b).has_value());
    REQUIRE(matrix.rows() == 2);

    // The row itself, a sum of stored rows, and the true equation are all in
    // the span and must be rejected without changing anything.
    CHECK(matrix.add_row_get_index(a) == std::nullopt);
    Equation sum = a;
    sum.plus_assign(b);
    CHECK(matrix.add_row_get_index(sum) == std::nullopt);
    CHECK(matrix.add_row_get_index(Equation::eq_true(repr, 5)) == std::nullopt);
    CHECK(matrix.rows() == 2);

    matrix.add_row(a);
    CHECK(matrix.rows() == 2);
    check_echelon_invariant(matrix);

    // The false equation is not in this span, and its leading column is the
    // constant column.
    CHECK(matrix.add_row_get_index(Equation::eq_false(repr, 5)) ==
          std::optional<std::size_t>(2));
    CHECK(matrix.pivot_columns().test(5));
    check_echelon_invariant(matrix);
}

TEST_CASE_TEMPLATE("row echelon: reduce and partial_reduce", Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    RowEchelonMatrix matrix(repr, 4);
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
        CHECK(matrix.pivot_columns().test(0) == true);
        for (const std::size_t column : bits_of(v)) {
            CHECK_FALSE(matrix.pivot_columns().test(column));
        }
    }

    SUBCASE("a reduction to the false equation is reported as eq_false") {
        RowEchelonMatrix contradictory(repr, 4);
        contradictory.add_row(make_equation_with(repr, 4, {0, 4}));
        Equation v = make_equation_with(repr, 4, {0});
        CHECK(contradictory.partial_reduce(v) == ReduceResult::eq_false());
        CHECK(v.is_eq_false());
    }
}

TEST_CASE_TEMPLATE("row echelon: reduce records its coefficients", Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    RowEchelonMatrix matrix(repr, 5);
    matrix.add_row(make_equation_with(repr, 5, {0, 3}));
    matrix.add_row(make_equation_with(repr, 5, {1, 4, 5}));
    matrix.add_row(make_equation_with(repr, 5, {2, 5}));
    REQUIRE(matrix.rows() == 3);

    Equation v = make_equation_with(repr, 5, {0, 2, 4});
    const Equation original = v;

    Bitmap coefficients;
    matrix.reduce(v, coefficients);
    CHECK(coefficients.size() == matrix.rows());

    // The recorded rows must sum to exactly the change reduction made.
    Equation rebuilt = original;
    coefficients.for_each_set([&](std::size_t index) { rebuilt.plus_assign(matrix.row(index)); });
    CHECK(rebuilt == v);
    CHECK(coefficients.count() > 0);
}

TEST_CASE_TEMPLATE("row echelon: partial_reduce_with_extra matches an augmented matrix", Tag,
                   DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    constexpr std::size_t num_vars = 12;
    TestRandom random(4242);

    for (int trial = 0; trial < 100; ++trial) {
        RowEchelonMatrix matrix(repr, num_vars);
        for (int i = 0; i < 4; ++i) {
            matrix.add_row(to_equation(repr, random_bits(random, num_vars, 3)));
        }

        // `extra` must be partially reduced by the matrix and non-zero, which is
        // exactly the precondition `partial_reduce_with_extra()` states.
        Equation extra = to_equation(repr, random_bits(random, num_vars, 3));
        if (matrix.partial_reduce(extra) == ReduceResult::eq_true()) {
            continue;
        }

        RowEchelonMatrix augmented = matrix;
        REQUIRE(augmented.add_row_get_index(extra).has_value());
        check_echelon_invariant(augmented);

        const Equation v = to_equation(repr, random_bits(random, num_vars, 3));

        Equation partial_with_extra = v;
        Equation partial_via_augmented = v;
        const ReduceResult a = matrix.partial_reduce_with_extra(partial_with_extra, extra);
        const ReduceResult b = augmented.partial_reduce(partial_via_augmented);
        CHECK(a == b);
        CHECK(partial_with_extra == partial_via_augmented);
    }
}

TEST_CASE_TEMPLATE("row echelon: partial_reduce_with_extra tolerates a zero extra", Tag, DenseTag,
                   SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    RowEchelonMatrix matrix(repr, 4);
    matrix.add_row(make_equation_with(repr, 4, {0, 2}));

    const Equation zero_extra = Equation::eq_true(repr, 4);
    Equation v = make_equation_with(repr, 4, {0, 3});
    Equation without = v;
    CHECK(matrix.partial_reduce_with_extra(v, zero_extra) == matrix.partial_reduce(without));
    CHECK(v == without);
}

TEST_CASE_TEMPLATE("row echelon: pop_first_row and pop_last_row", Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    RowEchelonMatrix matrix(repr, 6);
    matrix.add_row(make_equation_with(repr, 6, {0, 6}));
    matrix.add_row(make_equation_with(repr, 6, {2}));
    matrix.add_row(make_equation_with(repr, 6, {4, 5}));
    REQUIRE(matrix.rows() == 3);

    const Equation first = matrix.pop_first_row();
    CHECK(bits_of(first) == std::vector<std::size_t>{0, 6});
    CHECK(matrix.rows() == 2);
    CHECK_FALSE(matrix.pivot_columns().test(0));
    CHECK(bits_of(matrix.row(0)) == std::vector<std::size_t>{2});
    CHECK(bits_of(matrix.row(1)) == std::vector<std::size_t>{4, 5});
    check_echelon_invariant(matrix);

    matrix.pop_last_row();
    CHECK(matrix.rows() == 1);
    CHECK_FALSE(matrix.pivot_columns().test(4));
    CHECK(bits_of(matrix.row(0)) == std::vector<std::size_t>{2});
    check_echelon_invariant(matrix);

    matrix.pop_last_row();
    CHECK(matrix.rows() == 0);
    CHECK(matrix.pivot_columns().none());
    check_echelon_invariant(matrix);
}

TEST_CASE_TEMPLATE("row echelon: matches the reference matrix", Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    constexpr std::size_t num_vars = 17;
    TestRandom random(987654321);

    for (int trial = 0; trial < 60; ++trial) {
        RowEchelonMatrix matrix(repr, num_vars);
        ReferenceMatrix reference(num_vars);

        for (int step = 0; step < 40; ++step) {
            const Bits candidate = random_bits(random, num_vars, 4);
            const std::optional<std::size_t> actual =
                matrix.add_row_get_index(to_equation(repr, candidate));
            const std::optional<std::size_t> expected = reference.add_row_get_index(candidate);
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
        }

        while (matrix.rows() > 0) {
            if (random.chance(2)) {
                const Equation popped = matrix.pop_first_row();
                CHECK(to_bits(popped) == reference.pop_first_row());
            } else {
                matrix.pop_last_row();
                reference.pop_last_row();
            }
            check_matches_reference(matrix, reference);
            check_echelon_invariant(matrix);
        }
    }
}

TEST_CASE_TEMPLATE("row echelon: a reduced row never leads on a pivot column", Tag, DenseTag,
                   SparseTag) {
    // A random equation reduced against a random echelon basis must have a
    // leading column that is not a pivot column of that basis.
    constexpr EquationRepr repr = Tag::value;
    constexpr std::size_t num_vars = 24;
    TestRandom random(31337);

    for (int trial = 0; trial < 300; ++trial) {
        RowEchelonMatrix matrix(repr, num_vars);
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
                CHECK(matrix.pivot_columns().test(num_vars) == false);
                break;
            case ReduceResult::Kind::leading:
                CHECK(v.leading_bit() == std::optional<std::size_t>(result.bit));
                CHECK_FALSE(matrix.pivot_columns().test(result.bit));
                break;
        }

        Equation fully = original;
        matrix.reduce(fully);
        for (const std::size_t column : bits_of(fully)) {
            CHECK_FALSE(matrix.pivot_columns().test(column));
        }
        CHECK(fully.classify() == result);

        RowEchelonMatrix extended = matrix;
        CHECK(extended.add_row_get_index(original).has_value() ==
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
        REQUIRE_FALSE(space.pivot_columns().test(num_vars));

        const Bitmap solution = space.unique_solution();
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
    RowEchelonMatrix original(repr, 5);
    original.add_row(make_equation_with(repr, 5, {0, 5}));

    RowEchelonMatrix copy = original;
    copy.add_row(make_equation_with(repr, 5, {2}));
    CHECK(original.rows() == 1);
    CHECK(copy.rows() == 2);

    RowEchelonMatrix assigned(repr, 5);
    assigned = copy;
    CHECK(assigned.rows() == 2);
    assigned.pop_last_row();
    CHECK(copy.rows() == 2);
    check_echelon_invariant(original);
    check_echelon_invariant(copy);
    check_echelon_invariant(assigned);
}

TEST_CASE("row echelon: the two representations agree") {
    constexpr std::size_t num_vars = 29;
    TestRandom random(2718281828);

    RowEchelonMatrix dense(EquationRepr::dense, num_vars);
    RowEchelonMatrix sparse(EquationRepr::sparse, num_vars);

    for (int step = 0; step < 400; ++step) {
        const Bits candidate = random_bits(random, num_vars, 4);
        const std::optional<std::size_t> dense_index =
            dense.add_row_get_index(to_equation(EquationRepr::dense, candidate));
        const std::optional<std::size_t> sparse_index =
            sparse.add_row_get_index(to_equation(EquationRepr::sparse, candidate));
        REQUIRE(dense_index == sparse_index);
        REQUIRE(dense.rows() == sparse.rows());

        for (std::size_t index = 0; index < dense.rows(); ++index) {
            REQUIRE(dense.row(index).to_string() == sparse.row(index).to_string());
            REQUIRE(bits_of(dense.row(index)) == bits_of(sparse.row(index)));
        }
        REQUIRE(dense.pivot_columns() == sparse.pivot_columns());

        const Bits probe = random_bits(random, num_vars, 4);
        Equation dense_probe = to_equation(EquationRepr::dense, probe);
        Equation sparse_probe = to_equation(EquationRepr::sparse, probe);
        CHECK(dense.partial_reduce(dense_probe) == sparse.partial_reduce(sparse_probe));
        CHECK(bits_of(dense_probe) == bits_of(sparse_probe));
    }
}

} // namespace xorcle
