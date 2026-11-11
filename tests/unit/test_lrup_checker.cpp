#include <doctest/doctest.h>

#include <cstddef>
#include <fstream>
#include <initializer_list>
#include <sstream>
#include <string>
#include <vector>

#include "algebra/equation_repr.h"
#include "io/dimacs.h"
#include "io/proof_writer.h"
#include "io_test_support.h"

namespace xorcle {

namespace {

using Rows = std::vector<ProofRow>;

ProofRow row(std::initializer_list<std::size_t> bits) { return ProofRow(bits); }

} // namespace

// The mutants of `tests/test_check_lrup_parity.py`, so the two checkers stay in step.
TEST_CASE("lrup checker: a valid proof passes and each mutant fails") {
    // (x1 v x2), [x1 = 0] and [x2 = 0].
    const std::vector<Rows> inputs{Rows{row({0}), row({1})}, Rows{row({0, 2})},
                                   Rows{row({1, 2})}};
    const auto check = [&](const std::string& proof) {
        LrupChecker checker(EquationRepr::dense, 2, inputs);
        return checker.check(proof);
    };
    const auto rejects = [&](const std::string& proof, const std::string& message) {
        const std::string error = check(proof);
        CAPTURE(proof);
        CAPTURE(error);
        return error.contains(message);
    };
    // Learn x1 from clauses 3 and 1, delete 3, then the empty clause from 2 and 4.
    const std::string valid = "4 1 0 3 1 0\n3 0\n5 0 2 4 0\n";

    CHECK(check(valid) == "");
    CHECK(check("c a comment\n\n" + valid) == "");
    CHECK(check("4 0 2 3 1 0\n") == "");

    CHECK(rejects("4 1 0 1 0\n3 0\n5 0 2 4 0\n", "no conflict derived"));
    CHECK(rejects("4 -1 0 3 1 0\n3 0\n5 0 2 4 0\n", "did not propagate"));
    CHECK(rejects("4 1 0 3 1 0\n2 0\n5 0 2 4 0\n", "missing reason clause 2"));
    CHECK(rejects("4 1 0 3 1 0\n3 0\n", "no line derives the empty clause"));
    CHECK(rejects("", "no line derives the empty clause"));
    CHECK(rejects("4 1 0 3 1 0\n4 0 2 4 0\n", "does not exceed every earlier id"));
    CHECK(rejects("4 1 0 3 1 0\n3 0\n3 0 2 4 0\n", "does not exceed every earlier id"));
    CHECK(rejects("4 1 0 3 1 0\n9 0\n5 0 2 4 0\n", "which does not exist"));
    CHECK(rejects("4 1 0 3 1 0\n3 0\n3 0\n5 0 2 4 0\n", "which does not exist"));
    // A learned line cut short after its first 0 reads as a deletion.
    CHECK(rejects("4 1 0 3 1 0\n3 0\n5 0\n", "which does not exist"));
    CHECK(rejects("4 1 0\n", "which does not exist"));
    CHECK(rejects("0\n" + valid, "lists no clause id"));
    CHECK(rejects("4 1 0 3 1\n3 0\n5 0 2 4 0\n", "must end with 0"));
    CHECK(rejects("4 1 0 3 0 1 0\n", "exactly one or two zeros"));
}

// The cases `tests/test_check_lrup_parity.py` holds the Python checker to.
TEST_CASE("lrup checker: passes and fails the shared cases as the Python checker does") {
    std::ifstream file(XORCLE_TESTS_DIR "/lrup_cases.txt");
    REQUIRE(file.is_open());
    std::vector<std::string> lines;
    for (std::string line; std::getline(file, line);) {
        lines.push_back(line);
    }
    std::size_t num_cases = 0;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (!lines[i].starts_with("case ")) {
            continue;
        }
        std::istringstream header(lines[i]);
        std::string word;
        std::string name;
        std::string expect;
        header >> word >> name >> expect;
        CAPTURE(name);
        std::string instance;
        std::size_t j = i + 1;
        for (; lines[j] != "proof"; ++j) {
            instance += lines[j] + "\n";
        }
        std::string proof;
        for (++j; lines[j] != "end"; ++j) {
            proof += lines[j] + "\n";
        }
        i = j;
        const ParsedDimacs parsed = parse_dimacs_string(instance, EquationRepr::dense);
        LrupChecker checker(EquationRepr::dense, parsed.num_vars, clause_rows(parsed));
        CHECK(checker.check(proof).empty() == (expect == "pass"));
        ++num_cases;
    }
    CHECK(num_cases > 0);
}

} // namespace xorcle
