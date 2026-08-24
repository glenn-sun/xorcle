#include <doctest/doctest.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "algebra/equation_repr.h"
#include "algebra/row_echelon_matrix.h"
#include "algebra_test_support.h"
#include "core/clause_id.h"
#include "io/dimacs.h"
#include "io/lrup_logger.h"

namespace xorcle {

namespace {

/// @brief A proof path in the temp directory, unique to one test case.
std::filesystem::path proof_path(EquationRepr repr, std::string_view name) {
    const std::string suffix = repr == EquationRepr::dense ? "_dense.lrup" : "_sparse.lrup";
    return std::filesystem::temp_directory_path() /
           ("xorcle_lrup_test_" + std::string{name} + suffix);
}

/// @brief How many of the logger's temporary files sit next to `path`. The
/// logger names them `<final filename>.tmp.<counter>` in the same directory.
std::size_t count_temp_files(const std::filesystem::path& path) {
    const std::string prefix = path.filename().string() + ".tmp.";
    std::size_t found = 0;
    for (const auto& entry : std::filesystem::directory_iterator{path.parent_path()}) {
        if (entry.path().filename().string().starts_with(prefix)) {
            ++found;
        }
    }
    return found;
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream file{path};
    std::ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

/// @brief The single clause of a one-clause instance.
Clause clause_from(std::string_view line, std::size_t num_vars, EquationRepr repr) {
    ParsedDimacs parsed = parse_dimacs_string(
        "p cnf " + std::to_string(num_vars) + " 1\n" + std::string{line} + "\n", repr);
    REQUIRE(parsed.clauses.size() == 1);
    return std::move(parsed.clauses[0]);
}

} // namespace

TEST_CASE_TEMPLATE("lrup logger: learned and empty clause lines", Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    const std::filesystem::path path = proof_path(repr, "lines");

    {
        LrupLogger logger(path, 3);
        CHECK(logger.num_vars() == 3);

        logger.log_learned_clause(ClauseId{5}, clause_from("1 -2 0", 3, repr),
                                  std::vector<ClauseId>{0, 2, 3});
        logger.log_empty_clause(ClauseId{7}, std::vector<ClauseId>{1, 0});
        logger.finalize(true);
    }

    // Every id is written 1-based: learned id 5 is `6`, reasons 0,2,3 are `1 3 4`.
    CHECK(read_file(path) == "6 1 -2 0 1 3 4 0\n8 0 2 1 0\n");
    std::filesystem::remove(path);
}

TEST_CASE_TEMPLATE("lrup logger: tokens round-trip through the parser", Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    const std::filesystem::path path = proof_path(repr, "roundtrip");

    {
        LrupLogger logger(path, 4);
        logger.log_learned_clause(ClauseId{0}, clause_from("1+2 -3 0", 4, repr),
                                  std::vector<ClauseId>{4});
        logger.finalize(true);
    }

    // The token emitted for a row is the token that produced it, and rows come
    // out in row-echelon order.
    CHECK(read_file(path) == "1 1+2 -3 0 5 0\n");
    std::filesystem::remove(path);
}

TEST_CASE_TEMPLATE("lrup logger: rows are emitted in echelon order", Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    const std::filesystem::path path = proof_path(repr, "echelon");

    {
        LrupLogger logger(path, 3);
        logger.log_learned_clause(ClauseId{2}, clause_from("-3 1 0", 3, repr),
                                  std::vector<ClauseId>{1});
        logger.finalize(true);
    }

    CHECK(read_file(path) == "3 1 -3 0 2 0\n");
    std::filesystem::remove(path);
}

TEST_CASE_TEMPLATE("lrup logger: an empty reason list still closes the line", Tag, DenseTag,
                   SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    const std::filesystem::path path = proof_path(repr, "no_reasons");

    {
        LrupLogger logger(path, 2);
        logger.log_empty_clause(ClauseId{0}, std::vector<ClauseId>{});
        logger.finalize(true);
    }

    CHECK(read_file(path) == "1 0 0\n");
    std::filesystem::remove(path);
}

TEST_CASE_TEMPLATE("lrup logger: finalize(false) discards the proof", Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    const std::filesystem::path path = proof_path(repr, "discard");

    {
        LrupLogger logger(path, 2);
        logger.log_empty_clause(ClauseId{0}, std::vector<ClauseId>{0});
        CHECK(count_temp_files(path) == 1);
        logger.finalize(false);
    }

    CHECK_FALSE(std::filesystem::exists(path));
    CHECK(count_temp_files(path) == 0);
}

TEST_CASE_TEMPLATE("lrup logger: an abandoned logger leaves nothing behind", Tag, DenseTag,
                   SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    const std::filesystem::path path = proof_path(repr, "abandoned");

    {
        LrupLogger logger(path, 2);
        logger.log_empty_clause(ClauseId{0}, std::vector<ClauseId>{0});
    }

    CHECK_FALSE(std::filesystem::exists(path));
    CHECK(count_temp_files(path) == 0);
}

TEST_CASE_TEMPLATE("lrup logger: finalize is idempotent", Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    const std::filesystem::path path = proof_path(repr, "idempotent");

    {
        LrupLogger logger(path, 2);
        logger.log_empty_clause(ClauseId{0}, std::vector<ClauseId>{0});
        logger.finalize(true);
        logger.finalize(false);
    }

    CHECK(std::filesystem::exists(path));
    CHECK(read_file(path) == "1 0 1 0\n");
    std::filesystem::remove(path);
}

TEST_CASE("lrup logger: an unopenable proof path is an error") {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "xorcle_lrup_no_such_directory" / "out.lrup";

    CHECK_THROWS_AS(LrupLogger(path, 2), LrupError);
}

TEST_CASE("lrup logger: proofs and compaction are mutually exclusive") {
    CHECK_THROWS_AS(require_compaction_disabled_for_proof(true, true), LrupError);
    CHECK_NOTHROW(require_compaction_disabled_for_proof(true, false));
    CHECK_NOTHROW(require_compaction_disabled_for_proof(false, true));
    CHECK_NOTHROW(require_compaction_disabled_for_proof(false, false));

    try {
        require_compaction_disabled_for_proof(true, true);
        FAIL("expected an LrupError");
    } catch (const LrupError& error) {
        // The message has to name the fix.
        CHECK(std::string{error.what()}.find("--compact=false") != std::string::npos);
    }
}

} // namespace xorcle
