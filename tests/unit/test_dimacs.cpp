#include <doctest/doctest.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "algebra/equation_repr.h"
#include "algebra_test_support.h"
#include "io/dimacs.h"
#include "io_test_support.h"

namespace xorcle {

TEST_CASE_TEMPLATE("dimacs: a plain CNF instance", Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    const ParsedDimacs parsed = parse_dimacs_string("p cnf 3 2\n1 -2 0\n3 0\n", repr);

    CHECK(parsed.num_vars == 3);
    REQUIRE(parsed.clauses.size() == 2);
    CHECK(parsed.clauses[0].repr() == repr);
    CHECK(parsed.clauses[0].num_vars() == 3);

    // `1` is the literal [x1 = 1], stored as its linear negation [x1 = 0]; `-2`
    // is [x2 = 0], stored as [x2 = 1], i.e. with the constant column 3 set.
    CHECK(rows_of(parsed.clauses[0]) == std::vector<std::vector<std::size_t>>{{0}, {1, 3}});
    CHECK(rows_of(parsed.clauses[1]) == std::vector<std::vector<std::size_t>>{{2}});
}

TEST_CASE_TEMPLATE("dimacs: the clauses are over the used variables alone", Tag, DenseTag,
                   SparseTag) {
    constexpr EquationRepr repr = Tag::value;

    SUBCASE("every variable used keeps the declared numbering") {
        const ParsedDimacs parsed = parse_dimacs_string("p cnf 3 2\n1 -2 0\n3 0\n", repr);
        CHECK(parsed.used_vars == std::vector<std::size_t>{0, 1, 2});
    }

    SUBCASE("a variable no clause uses is dropped, and the rest keep their order") {
        const ParsedDimacs parsed =
            parse_dimacs_string("p cnf 9 3\n8 -2 0\n2+5 0\nx5 -8 0\n", repr);
        CHECK(parsed.num_vars == 9);
        CHECK(parsed.used_vars == std::vector<std::size_t>{1, 4, 7});
        REQUIRE(parsed.clauses.size() == 3);
        CHECK(parsed.clauses[0].num_vars() == 3);
        // The constant bit follows the used variables.
        CHECK(rows_of(parsed.clauses[0]) == std::vector<std::vector<std::size_t>>{{0, 3}, {2}});
        CHECK(rows_of(parsed.clauses[1]) == std::vector<std::vector<std::size_t>>{{0, 1}});
        CHECK(rows_of(parsed.clauses[2]) == std::vector<std::vector<std::size_t>>{{1, 2, 3}});
    }

    SUBCASE("a variable that cancels out of its token is unused") {
        const ParsedDimacs parsed = parse_dimacs_string("p cnf 4 1\n2+2+3 4 0\n", repr);
        CHECK(parsed.used_vars == std::vector<std::size_t>{2, 3});
        CHECK(rows_of(parsed.clauses[0]) == std::vector<std::vector<std::size_t>>{{0}, {1}});
    }

    SUBCASE("no used variable leaves only the constant bit") {
        const ParsedDimacs parsed = parse_dimacs_string("p cnf 4 2\n0\nx 0\n", repr);
        CHECK(parsed.used_vars.empty());
        REQUIRE(parsed.clauses.size() == 2);
        CHECK(parsed.clauses[0].num_vars() == 0);
        CHECK(parsed.clauses[0].rows() == 0);
        CHECK(parsed.clauses[1].rows() == 0);
    }
}

TEST_CASE_TEMPLATE("dimacs: XOR tokens and the xnf header", Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    const ParsedDimacs parsed = parse_dimacs_string("p xnf 4 1\n1+2 -3+4 0\n", repr);

    CHECK(parsed.num_vars == 4);
    REQUIRE(parsed.clauses.size() == 1);
    CHECK(rows_of(parsed.clauses[0]) == std::vector<std::vector<std::size_t>>{{0, 1}, {2, 3, 4}});
}

TEST_CASE_TEMPLATE("dimacs: comments, blank lines and CRLF", Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    const ParsedDimacs parsed = parse_dimacs_string(
        "c leading comment\r\n"
        "\r\n"
        "   c an indented comment\r\n"
        "p cnf 2 1\r\n"
        "c between clauses\r\n"
        "1 2 0\r\n",
        repr);

    CHECK(parsed.num_vars == 2);
    REQUIRE(parsed.clauses.size() == 1);
    CHECK(rows_of(parsed.clauses[0]) == std::vector<std::vector<std::size_t>>{{0}, {1}});
}

TEST_CASE_TEMPLATE("dimacs: rows are echelonized, not stored verbatim", Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;

    SUBCASE("a repeated literal collapses") {
        const ParsedDimacs parsed = parse_dimacs_string("p cnf 2 1\n2 2 0\n", repr);
        REQUIRE(parsed.clauses.size() == 1);
        CHECK(clause_rows(parsed)[0] == std::vector<std::vector<std::size_t>>{{1}});
    }

    SUBCASE("a dependent literal collapses") {
        const ParsedDimacs parsed = parse_dimacs_string("p cnf 3 1\n1 2 1+2 0\n", repr);
        REQUIRE(parsed.clauses.size() == 1);
        CHECK(rows_of(parsed.clauses[0]) == std::vector<std::vector<std::size_t>>{{0}, {1}});
    }

    SUBCASE("source order does not survive") {
        const ParsedDimacs parsed = parse_dimacs_string("p cnf 3 1\n3 1 0\n", repr);
        REQUIRE(parsed.clauses.size() == 1);
        CHECK(clause_rows(parsed)[0] == std::vector<std::vector<std::size_t>>{{0}, {2}});
    }

    SUBCASE("a variable repeated inside one XOR token cancels") {
        // `x1 + x1 = 1` is `0 = 1`, whose linear negation is the zero space.
        const ParsedDimacs parsed = parse_dimacs_string("p cnf 2 1\n1+1 0\n", repr);
        REQUIRE(parsed.clauses.size() == 1);
        CHECK(rows_of(parsed.clauses[0]).empty());
    }

    SUBCASE("a variable repeated an odd number of times survives once") {
        const ParsedDimacs parsed = parse_dimacs_string("p cnf 2 1\n1+2+1+1 0\n", repr);
        REQUIRE(parsed.clauses.size() == 1);
        CHECK(rows_of(parsed.clauses[0]) == std::vector<std::vector<std::size_t>>{{0, 1}});
    }
}

// Parse errors are thrown before any representation matters.
TEST_CASE("dimacs: a missing header is an error") {
    constexpr EquationRepr repr = EquationRepr::dense;

    CHECK_THROWS_AS(parse_dimacs_string("", repr), DimacsError);
    CHECK_THROWS_AS(parse_dimacs_string("c only comments\n\n", repr), DimacsError);
    CHECK_THROWS_AS(parse_dimacs_string("1 2 0\n", repr), DimacsError);
}

TEST_CASE("dimacs: malformed headers are errors") {
    constexpr EquationRepr repr = EquationRepr::dense;

    CHECK_THROWS_AS(parse_dimacs_string("p cnf 3\n", repr), DimacsError);
    CHECK_THROWS_AS(parse_dimacs_string("p cnf 3 1 extra\n1 0\n", repr), DimacsError);
    CHECK_THROWS_AS(parse_dimacs_string("q cnf 3 1\n1 0\n", repr), DimacsError);
    CHECK_THROWS_AS(parse_dimacs_string("p dnf 3 1\n1 0\n", repr), DimacsError);
    CHECK_THROWS_AS(parse_dimacs_string("p cnf x 1\n1 0\n", repr), DimacsError);
    CHECK_THROWS_AS(parse_dimacs_string("p cnf 3 -1\n1 0\n", repr), DimacsError);
}

TEST_CASE("dimacs: a wrong header clause count is an error") {
    constexpr EquationRepr repr = EquationRepr::dense;

    CHECK_THROWS_AS(parse_dimacs_string("p cnf 3 3\n1 0\n2 0\n", repr), DimacsError);
    CHECK_THROWS_AS(parse_dimacs_string("p cnf 3 1\n1 0\n2 0\n", repr), DimacsError);
}

TEST_CASE("dimacs: a clause line must end with 0") {
    constexpr EquationRepr repr = EquationRepr::dense;

    CHECK_THROWS_AS(parse_dimacs_string("p cnf 3 1\n1 2\n", repr), DimacsError);
    CHECK_THROWS_AS(parse_dimacs_string("p cnf 3 2\n1 0\n2 3\n", repr), DimacsError);

    // Two clauses on one line are rejected, not truncated to the first.
    CHECK_THROWS_AS(parse_dimacs_string("p cnf 3 2\n1 0 2 0\n", repr), DimacsError);
}

TEST_CASE("dimacs: out-of-range variables are errors") {
    constexpr EquationRepr repr = EquationRepr::dense;

    CHECK_THROWS_AS(parse_dimacs_string("p cnf 3 1\n4 0\n", repr), DimacsError);
    CHECK_THROWS_AS(parse_dimacs_string("p cnf 3 1\n-4 0\n", repr), DimacsError);
    CHECK_THROWS_AS(parse_dimacs_string("p cnf 3 1\n1+9 0\n", repr), DimacsError);
    CHECK_THROWS_AS(parse_dimacs_string("p cnf 3 1\n-0 0\n", repr), DimacsError);
}

TEST_CASE("dimacs: junk tokens are errors") {
    constexpr EquationRepr repr = EquationRepr::dense;

    CHECK_THROWS_AS(parse_dimacs_string("p cnf 3 1\nhello 0\n", repr), DimacsError);
    CHECK_THROWS_AS(parse_dimacs_string("p cnf 3 1\n1x 0\n", repr), DimacsError);
    CHECK_THROWS_AS(parse_dimacs_string("p cnf 3 1\n1.5 0\n", repr), DimacsError);
    CHECK_THROWS_AS(parse_dimacs_string("p cnf 3 1\n1+ 0\n", repr), DimacsError);
    CHECK_THROWS_AS(parse_dimacs_string("p cnf 3 1\n+1 0\n", repr), DimacsError);
    CHECK_THROWS_AS(parse_dimacs_string("p cnf 3 1\n1++2 0\n", repr), DimacsError);
    CHECK_THROWS_AS(parse_dimacs_string("p cnf 3 1\n1+-2 0\n", repr), DimacsError);
    CHECK_THROWS_AS(parse_dimacs_string("p cnf 3 1\n--1 0\n", repr), DimacsError);
    CHECK_THROWS_AS(parse_dimacs_string("p cnf 3 1\n- 0\n", repr), DimacsError);
    CHECK_THROWS_AS(parse_dimacs_string("p cnf 3 1\n-+1 0\n", repr), DimacsError);
}

TEST_CASE_TEMPLATE("dimacs: CNF-XOR lines", Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    using Rows = std::vector<std::vector<std::size_t>>;

    SUBCASE("the literals XOR to 1") {
        // `x1 2 3 0` is [x1 + x2 + x3 = 1], stored as [x1 + x2 + x3 = 0].
        const ParsedDimacs parsed = parse_dimacs_string("p cnf 3 1\nx1 2 3 0\n", repr);
        REQUIRE(parsed.clauses.size() == 1);
        CHECK(rows_of(parsed.clauses[0]) == Rows{{0, 1, 2}});
    }

    SUBCASE("each negated literal flips the constant") {
        const ParsedDimacs one = parse_dimacs_string("p cnf 3 1\nx1 -2 3 0\n", repr);
        REQUIRE(one.clauses.size() == 1);
        CHECK(rows_of(one.clauses[0]) == Rows{{0, 1, 2, 3}});

        const ParsedDimacs two = parse_dimacs_string("p cnf 3 1\nx-1 -2 3 0\n", repr);
        REQUIRE(two.clauses.size() == 1);
        CHECK(rows_of(two.clauses[0]) == Rows{{0, 1, 2}});
    }

    SUBCASE("an XOR line is the single-equation clause XNF writes with '+'") {
        const ParsedDimacs xor_line = parse_dimacs_string("p cnf 3 1\nx1 -2 3 0\n", repr);
        const ParsedDimacs xnf_line = parse_dimacs_string("p cnf 3 1\n-1+2+3 0\n", repr);
        REQUIRE(xor_line.clauses.size() == 1);
        REQUIRE(xnf_line.clauses.size() == 1);
        CHECK(rows_of(xor_line.clauses[0]) == rows_of(xnf_line.clauses[0]));
    }

    SUBCASE("a single literal is a unit") {
        const ParsedDimacs parsed = parse_dimacs_string("p cnf 2 2\nx2 0\nx-2 0\n", repr);
        REQUIRE(parsed.clauses.size() == 2);
        CHECK(clause_rows(parsed)[0] == Rows{{1}});
        CHECK(clause_rows(parsed)[1] == Rows{{1, 2}});
    }

    SUBCASE("whitespace after and before the x is allowed") {
        const ParsedDimacs parsed =
            parse_dimacs_string("p cnf 3 2\nx -1 2 0\n  x-1 2 0\n", repr);
        REQUIRE(parsed.clauses.size() == 2);
        CHECK(clause_rows(parsed)[0] == Rows{{0, 1, 3}});
        CHECK(clause_rows(parsed)[1] == Rows{{0, 1, 3}});
    }

    SUBCASE("a repeated variable cancels, and its negations still count") {
        const ParsedDimacs parsed =
            parse_dimacs_string("p cnf 3 2\nx1 2 1 0\nx1 2 -1 0\n", repr);
        REQUIRE(parsed.clauses.size() == 2);
        CHECK(clause_rows(parsed)[0] == Rows{{1}});
        CHECK(clause_rows(parsed)[1] == Rows{{1, 3}});
    }

    SUBCASE("x1 -1 is 0 = 0, stored as 0 = 1") {
        const ParsedDimacs parsed = parse_dimacs_string("p cnf 2 1\nx1 -1 0\n", repr);
        REQUIRE(parsed.clauses.size() == 1);
        CHECK(clause_rows(parsed)[0] == Rows{{2}});
    }

    SUBCASE("an empty XOR is 0 = 1, the empty clause") {
        const ParsedDimacs parsed = parse_dimacs_string("p cnf 2 2\nx 0\nx0\n", repr);
        REQUIRE(parsed.clauses.size() == 2);
        CHECK(rows_of(parsed.clauses[0]).empty());
        CHECK(rows_of(parsed.clauses[1]).empty());
    }

    SUBCASE("XOR lines and clause lines keep file order") {
        const ParsedDimacs parsed =
            parse_dimacs_string("p cnf 3 3\n1 2 0\nx1 3 0\n1+2 -3 0\n", repr);
        REQUIRE(parsed.clauses.size() == 3);
        CHECK(rows_of(parsed.clauses[0]) == Rows{{0}, {1}});
        CHECK(rows_of(parsed.clauses[1]) == Rows{{0, 2}});
        CHECK(rows_of(parsed.clauses[2]) == Rows{{0, 1}, {2, 3}});
    }
}

TEST_CASE_TEMPLATE("dimacs: the header count may include or exclude XOR lines", Tag, DenseTag,
                   SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    const std::string body = "x1 2 0\n3 0\nx-2 3 0\n";

    CHECK(parse_dimacs_string("p cnf 3 3\n" + body, repr).clauses.size() == 3);
    CHECK(parse_dimacs_string("p cnf 3 1\n" + body, repr).clauses.size() == 3);
    CHECK(parse_dimacs_string("p cnf 3 0\nx1 2 0\n", repr).clauses.size() == 1);

    CHECK_THROWS_AS(parse_dimacs_string("p cnf 3 0\n" + body, repr), DimacsError);
    CHECK_THROWS_AS(parse_dimacs_string("p cnf 3 2\n" + body, repr), DimacsError);
    CHECK_THROWS_AS(parse_dimacs_string("p cnf 3 4\n" + body, repr), DimacsError);
}

TEST_CASE("dimacs: malformed XOR lines are errors") {
    constexpr EquationRepr repr = EquationRepr::dense;

    CHECK_THROWS_AS(parse_dimacs_string("p cnf 3 1\nx1 2\n", repr), DimacsError);
    CHECK_THROWS_AS(parse_dimacs_string("p cnf 3 1\nx\n", repr), DimacsError);
    CHECK_THROWS_AS(parse_dimacs_string("p cnf 3 1\nx1 2 0 3\n", repr), DimacsError);
    CHECK_THROWS_AS(parse_dimacs_string("p cnf 3 1\nx1 2 0 0\n", repr), DimacsError);
    CHECK_THROWS_AS(parse_dimacs_string("p cnf 3 1\nx4 0\n", repr), DimacsError);
    CHECK_THROWS_AS(parse_dimacs_string("p cnf 3 1\nx1 -4 0\n", repr), DimacsError);
    CHECK_THROWS_AS(parse_dimacs_string("p cnf 3 1\nx1 -0 0\n", repr), DimacsError);
    CHECK_THROWS_AS(parse_dimacs_string("p cnf 3 1\nx1+2 0\n", repr), DimacsError);
    CHECK_THROWS_AS(parse_dimacs_string("p cnf 3 1\nx+1 0\n", repr), DimacsError);
    CHECK_THROWS_AS(parse_dimacs_string("p cnf 3 1\nxx1 0\n", repr), DimacsError);
    CHECK_THROWS_AS(parse_dimacs_string("p cnf 3 1\nxor 1 2 0\n", repr), DimacsError);
    CHECK_THROWS_AS(parse_dimacs_string("p cnf 3 1\nx1.5 0\n", repr), DimacsError);
    CHECK_THROWS_AS(parse_dimacs_string("x1 2 0\np cnf 3 1\n", repr), DimacsError);
}

TEST_CASE("dimacs: errors name the input and the line") {
    constexpr EquationRepr repr = EquationRepr::dense;

    try {
        parse_dimacs_string("p cnf 3 1\nc a comment\n9 0\n", repr, "instance.xnf");
        FAIL("expected a DimacsError");
    } catch (const DimacsError& error) {
        CHECK(std::string{error.what()}.starts_with("instance.xnf:3:"));
    }
}

TEST_CASE_TEMPLATE("dimacs: the stream and file forms agree", Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    const std::string text = "p cnf 3 2\n1+2 0\n-3 0\n";
    const std::filesystem::path path =
        scratch_path(repr == EquationRepr::dense ? "dimacs_dense.xnf" : "dimacs_sparse.xnf");

    {
        std::ofstream file{path};
        REQUIRE(file.is_open());
        file << text;
    }

    const ParsedDimacs from_file = parse_dimacs_file(path, repr);
    const ParsedDimacs from_string = parse_dimacs_string(text, repr);

    CHECK(from_file.num_vars == from_string.num_vars);
    REQUIRE(from_file.clauses.size() == from_string.clauses.size());
    for (std::size_t i = 0; i < from_file.clauses.size(); ++i) {
        CHECK(rows_of(from_file.clauses[i]) == rows_of(from_string.clauses[i]));
    }

    std::filesystem::remove(path);
}

TEST_CASE("dimacs: an unopenable file is an error") {
    constexpr EquationRepr repr = EquationRepr::dense;
    const std::filesystem::path path = scratch_path("no_such_directory/missing.xnf");

    CHECK_THROWS_AS(parse_dimacs_file(path, repr), DimacsError);
}

} // namespace xorcle
