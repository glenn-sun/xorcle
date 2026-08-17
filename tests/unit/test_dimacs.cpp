#include <doctest/doctest.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "algebra/algebra.h"
#include "algebra_test_support.h"
#include "io/dimacs.h"

namespace xorcle {

namespace {

/// @brief The set columns of every row of a clause, in row-echelon order.
std::vector<std::vector<std::size_t>> rows_of(const Clause& clause) {
    std::vector<std::vector<std::size_t>> out;
    for (std::size_t i = 0; i < clause.rows(); ++i) {
        out.push_back(columns_of(clause.row(i)));
    }
    return out;
}

/// @brief A path in the temp directory, unique to one test case.
std::filesystem::path scratch_path(std::string_view name) {
    return std::filesystem::temp_directory_path() / ("xorcle_dimacs_test_" + std::string{name});
}

} // namespace

TEST_CASE_TEMPLATE("dimacs: a plain CNF instance", Tag, DenseTag, SparseTag) {
    constexpr Algebra backend = Tag::value;
    const ParsedDimacs parsed = parse_dimacs_string("p cnf 3 2\n1 -2 0\n3 0\n", backend);

    CHECK(parsed.num_vars == 3);
    REQUIRE(parsed.clauses.size() == 2);
    CHECK(parsed.clauses[0].backend() == backend);
    CHECK(parsed.clauses[0].num_vars() == 3);

    // `1` is the literal [x1 = 1], stored as its linear negation [x1 = 0]; `-2`
    // is [x2 = 0], stored as [x2 = 1], i.e. with the constant column 3 set.
    CHECK(rows_of(parsed.clauses[0]) == std::vector<std::vector<std::size_t>>{{0}, {1, 3}});
    CHECK(rows_of(parsed.clauses[1]) == std::vector<std::vector<std::size_t>>{{2}});
}

TEST_CASE_TEMPLATE("dimacs: XOR tokens and the xnf header", Tag, DenseTag, SparseTag) {
    constexpr Algebra backend = Tag::value;
    const ParsedDimacs parsed = parse_dimacs_string("p xnf 4 1\n1+2 -3+4 0\n", backend);

    CHECK(parsed.num_vars == 4);
    REQUIRE(parsed.clauses.size() == 1);
    CHECK(rows_of(parsed.clauses[0]) == std::vector<std::vector<std::size_t>>{{0, 1}, {2, 3, 4}});
}

TEST_CASE_TEMPLATE("dimacs: comments, blank lines and CRLF", Tag, DenseTag, SparseTag) {
    constexpr Algebra backend = Tag::value;
    const ParsedDimacs parsed = parse_dimacs_string(
        "c leading comment\r\n"
        "\r\n"
        "   c an indented comment\r\n"
        "p cnf 2 1\r\n"
        "c between clauses\r\n"
        "1 2 0\r\n",
        backend);

    CHECK(parsed.num_vars == 2);
    REQUIRE(parsed.clauses.size() == 1);
    CHECK(rows_of(parsed.clauses[0]) == std::vector<std::vector<std::size_t>>{{0}, {1}});
}

TEST_CASE_TEMPLATE("dimacs: rows are echelonized, not stored verbatim", Tag, DenseTag, SparseTag) {
    constexpr Algebra backend = Tag::value;

    SUBCASE("a repeated literal collapses") {
        const ParsedDimacs parsed = parse_dimacs_string("p cnf 2 1\n2 2 0\n", backend);
        REQUIRE(parsed.clauses.size() == 1);
        CHECK(rows_of(parsed.clauses[0]) == std::vector<std::vector<std::size_t>>{{1}});
    }

    SUBCASE("a dependent literal collapses") {
        const ParsedDimacs parsed = parse_dimacs_string("p cnf 3 1\n1 2 1+2 0\n", backend);
        REQUIRE(parsed.clauses.size() == 1);
        CHECK(rows_of(parsed.clauses[0]) == std::vector<std::vector<std::size_t>>{{0}, {1}});
    }

    SUBCASE("source order does not survive") {
        const ParsedDimacs parsed = parse_dimacs_string("p cnf 3 1\n3 1 0\n", backend);
        REQUIRE(parsed.clauses.size() == 1);
        CHECK(rows_of(parsed.clauses[0]) == std::vector<std::vector<std::size_t>>{{0}, {2}});
    }

    SUBCASE("a repeated variable inside one XOR token is idempotent, not cancelling") {
        const ParsedDimacs parsed = parse_dimacs_string("p cnf 2 1\n1+1 0\n", backend);
        REQUIRE(parsed.clauses.size() == 1);
        CHECK(rows_of(parsed.clauses[0]) == std::vector<std::vector<std::size_t>>{{0}});
    }
}

TEST_CASE_TEMPLATE("dimacs: a missing header is an error", Tag, DenseTag, SparseTag) {
    constexpr Algebra backend = Tag::value;

    CHECK_THROWS_AS(parse_dimacs_string("", backend), DimacsError);
    CHECK_THROWS_AS(parse_dimacs_string("c only comments\n\n", backend), DimacsError);
    CHECK_THROWS_AS(parse_dimacs_string("1 2 0\n", backend), DimacsError);
}

TEST_CASE_TEMPLATE("dimacs: malformed headers are errors", Tag, DenseTag, SparseTag) {
    constexpr Algebra backend = Tag::value;

    CHECK_THROWS_AS(parse_dimacs_string("p cnf 3\n", backend), DimacsError);
    CHECK_THROWS_AS(parse_dimacs_string("p cnf 3 1 extra\n1 0\n", backend), DimacsError);
    CHECK_THROWS_AS(parse_dimacs_string("q cnf 3 1\n1 0\n", backend), DimacsError);
    CHECK_THROWS_AS(parse_dimacs_string("p dnf 3 1\n1 0\n", backend), DimacsError);
    CHECK_THROWS_AS(parse_dimacs_string("p cnf x 1\n1 0\n", backend), DimacsError);
    CHECK_THROWS_AS(parse_dimacs_string("p cnf 3 -1\n1 0\n", backend), DimacsError);
}

TEST_CASE_TEMPLATE("dimacs: a wrong header clause count is an error", Tag, DenseTag, SparseTag) {
    constexpr Algebra backend = Tag::value;

    CHECK_THROWS_AS(parse_dimacs_string("p cnf 3 3\n1 0\n2 0\n", backend), DimacsError);
    CHECK_THROWS_AS(parse_dimacs_string("p cnf 3 1\n1 0\n2 0\n", backend), DimacsError);
}

TEST_CASE_TEMPLATE("dimacs: a clause line must end with 0", Tag, DenseTag, SparseTag) {
    constexpr Algebra backend = Tag::value;

    CHECK_THROWS_AS(parse_dimacs_string("p cnf 3 1\n1 2\n", backend), DimacsError);
    CHECK_THROWS_AS(parse_dimacs_string("p cnf 3 2\n1 0\n2 3\n", backend), DimacsError);

    // Rejected rather than dropped: silently ignoring the rest of the line would
    // lose the second clause of a two-clause line.
    CHECK_THROWS_AS(parse_dimacs_string("p cnf 3 2\n1 0 2 0\n", backend), DimacsError);
}

TEST_CASE_TEMPLATE("dimacs: out-of-range variables are errors", Tag, DenseTag, SparseTag) {
    constexpr Algebra backend = Tag::value;

    CHECK_THROWS_AS(parse_dimacs_string("p cnf 3 1\n4 0\n", backend), DimacsError);
    CHECK_THROWS_AS(parse_dimacs_string("p cnf 3 1\n-4 0\n", backend), DimacsError);
    CHECK_THROWS_AS(parse_dimacs_string("p cnf 3 1\n1+9 0\n", backend), DimacsError);
    CHECK_THROWS_AS(parse_dimacs_string("p cnf 3 1\n-0 0\n", backend), DimacsError);
}

TEST_CASE_TEMPLATE("dimacs: junk tokens are errors", Tag, DenseTag, SparseTag) {
    constexpr Algebra backend = Tag::value;

    CHECK_THROWS_AS(parse_dimacs_string("p cnf 3 1\nhello 0\n", backend), DimacsError);
    CHECK_THROWS_AS(parse_dimacs_string("p cnf 3 1\n1x 0\n", backend), DimacsError);
    CHECK_THROWS_AS(parse_dimacs_string("p cnf 3 1\n1.5 0\n", backend), DimacsError);
    CHECK_THROWS_AS(parse_dimacs_string("p cnf 3 1\n1+ 0\n", backend), DimacsError);
    CHECK_THROWS_AS(parse_dimacs_string("p cnf 3 1\n+1 0\n", backend), DimacsError);
    CHECK_THROWS_AS(parse_dimacs_string("p cnf 3 1\n1++2 0\n", backend), DimacsError);
    CHECK_THROWS_AS(parse_dimacs_string("p cnf 3 1\n1+-2 0\n", backend), DimacsError);
    CHECK_THROWS_AS(parse_dimacs_string("p cnf 3 1\n--1 0\n", backend), DimacsError);
    CHECK_THROWS_AS(parse_dimacs_string("p cnf 3 1\n- 0\n", backend), DimacsError);
    CHECK_THROWS_AS(parse_dimacs_string("p cnf 3 1\n-+1 0\n", backend), DimacsError);
}

TEST_CASE_TEMPLATE("dimacs: errors name the input and the line", Tag, DenseTag, SparseTag) {
    constexpr Algebra backend = Tag::value;

    try {
        parse_dimacs_string("p cnf 3 1\nc a comment\n9 0\n", backend, "instance.xnf");
        FAIL("expected a DimacsError");
    } catch (const DimacsError& error) {
        CHECK(error.line_number() == 3);
        CHECK(std::string{error.what()}.starts_with("instance.xnf:3:"));
    }
}

TEST_CASE_TEMPLATE("dimacs: the stream and file forms agree", Tag, DenseTag, SparseTag) {
    constexpr Algebra backend = Tag::value;
    const std::string text = "p cnf 3 2\n1+2 0\n-3 0\n";
    const std::filesystem::path path =
        scratch_path(backend == Algebra::dense ? "dense.xnf" : "sparse.xnf");

    {
        std::ofstream file{path};
        REQUIRE(file.is_open());
        file << text;
    }

    const ParsedDimacs from_file = parse_dimacs_file(path, backend);
    const ParsedDimacs from_string = parse_dimacs_string(text, backend);

    CHECK(from_file.num_vars == from_string.num_vars);
    REQUIRE(from_file.clauses.size() == from_string.clauses.size());
    for (std::size_t i = 0; i < from_file.clauses.size(); ++i) {
        CHECK(rows_of(from_file.clauses[i]) == rows_of(from_string.clauses[i]));
    }

    std::filesystem::remove(path);
}

TEST_CASE_TEMPLATE("dimacs: an unopenable file is an error", Tag, DenseTag, SparseTag) {
    constexpr Algebra backend = Tag::value;
    const std::filesystem::path path = scratch_path("no_such_directory/missing.xnf");

    CHECK_THROWS_AS(parse_dimacs_file(path, backend), DimacsError);
}

} // namespace xorcle
