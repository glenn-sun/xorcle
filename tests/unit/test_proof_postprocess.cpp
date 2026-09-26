#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "algebra/equation_repr.h"
#include "algebra_test_support.h"
#include "death_test.h"
#include "io/proof_postprocess.h"
#include "io/proof_raw.h"
#include "io_test_support.h"

namespace xorcle {

namespace {

using Rows = std::vector<RawRow>;

RawRow row(std::initializer_list<std::size_t> bits) { return RawRow(bits); }

/// @brief One hand-built raw proof and the instance it is about.
struct Case {
    std::size_t num_vars;
    std::vector<Rows> inputs;
    std::vector<RawRecord> records;
};

/// @brief Post-process `test_case` and check the proof it writes.
std::string run(EquationRepr repr, const Case& test_case, PostprocessSummary& summary,
                std::string& text) {
    RawRecordVectorSource source(test_case.records);
    std::ostringstream out;
    summary = postprocess_raw_proof(source, test_case.num_vars, test_case.inputs.size(), repr, out,
                                    test_case.inputs);
    text = out.str();
    LrupChecker checker(repr, test_case.num_vars, test_case.inputs);
    return checker.check(text);
}

/// @brief The ids on one proof line.
struct Line {
    bool learned = false;
    std::size_t id = 0;              ///< A learned line's own.
    std::vector<std::size_t> cited;  ///< A learned line's reasons, or a deletion line's ids.
};

std::vector<Line> parse_lines(const std::string& text) {
    std::vector<Line> lines;
    std::istringstream text_stream(text);
    for (std::string raw; std::getline(text_stream, raw);) {
        std::vector<std::string> tokens;
        std::istringstream line_stream(raw);
        for (std::string token; line_stream >> token;) {
            tokens.push_back(token);
        }
        Line line;
        line.learned = std::count(tokens.begin(), tokens.end(), "0") == 2;
        auto token = tokens.begin();
        if (line.learned) {
            line.id = std::stoul(*token);
            token = std::find(tokens.begin(), tokens.end(), "0") + 1;
        }
        for (; *token != "0"; ++token) {
            line.cited.push_back(std::stoul(*token));
        }
        lines.push_back(std::move(line));
    }
    return lines;
}

/// @brief Check what post-processing promises of every proof, whatever the
/// derivations: learned ids run densely on from the inputs, every learned line
/// but the last is cited later, and each clause is deleted exactly once, right
/// after the line that last cites it, or first thing if no line does.
void check_proof_shape(const std::string& text, std::size_t num_inputs) {
    const std::vector<Line> lines = parse_lines(text);
    REQUIRE_FALSE(lines.empty());

    std::size_t next_id = num_inputs + 1;
    std::map<std::size_t, std::size_t> last_citation; // id -> line index
    for (std::size_t index = 0; index < lines.size(); ++index) {
        if (lines[index].learned) {
            CHECK(lines[index].id == next_id++);
            for (const std::size_t id : lines[index].cited) {
                last_citation[id] = index;
            }
        }
    }
    for (std::size_t index = 0; index < lines.size(); ++index) {
        if (lines[index].learned && lines[index].id + 1 != next_id) {
            CHECK_MESSAGE(last_citation.contains(lines[index].id),
                          "line ", lines[index].id, " is never cited");
        }
    }

    std::map<std::size_t, std::size_t> deleted_at; // id -> line index
    for (std::size_t index = 0; index < lines.size(); ++index) {
        if (!lines[index].learned) {
            for (const std::size_t id : lines[index].cited) {
                CHECK_MESSAGE(deleted_at.emplace(id, index).second, "id ", id,
                              " is deleted twice");
            }
        }
    }
    for (const auto& [id, index] : last_citation) {
        REQUIRE_MESSAGE(deleted_at.contains(id), "cited id ", id, " is never deleted");
        CHECK(deleted_at.at(id) == index + 1);
    }
    for (const auto& [id, index] : deleted_at) {
        if (!last_citation.contains(id)) {
            CHECK_MESSAGE(id <= num_inputs, "uncited line ", id, " is deleted");
            CHECK(index == 0);
        }
    }
}

// (x1) & (~x1 | x2) & (~x2), plus an unused clause. No compaction anywhere.
Case uncompacted_case() {
    Case test_case;
    test_case.num_vars = 2;
    test_case.inputs = {Rows{row({0})},                 // 1: [x1 = 1]
                        Rows{row({0, 2}), row({1})},    // 2: [x1 = 0] v [x2 = 1]
                        Rows{row({1, 2})},              // 3: [x2 = 0]
                        Rows{row({0}), row({1})}};      // 4: unused
    test_case.records = {RawLearned{4, Rows{row({0})}, {0}},
                         RawDeleted{{4}},
                         RawLearned{5, Rows{row({1})}, {0, 1}},
                         RawEmpty{6, {5, 2}}};
    return test_case;
}

/// @brief One root epoch eliminating x2 = 0 and x1 = 1, over four variables.
///
/// The root trail is x2 = 0 from clause 2, which is already the unit line, then
/// x1 = 1 from clause 1, whose line 6 needs the first. Clause 3 then holds
/// `x1 = 1` and `x1 + x4 = 0`, so it propagates only modulo the second unit.
Case compacted_case(std::vector<ClauseId> learned_reasons) {
    Case test_case;
    test_case.num_vars = 4;
    test_case.inputs = {Rows{row({1}), row({0})},                    // 1: [x2 = 1] v [x1 = 1]
                        Rows{row({1, 4})},                           // 2: [x2 = 0]
                        Rows{row({0, 4}), row({0, 3}), row({2})},    // 3
                        Rows{row({2}), row({3})},                    // 4: [x3 = 1] v [x4 = 1]
                        Rows{row({2, 4}), row({3})},                 // 5: [x3 = 0] v [x4 = 1]
                        Rows{row({2, 4}), row({3, 4})}};             // 6: [x3 = 0] v [x4 = 0]
    test_case.records = {
        RawLearned{6, Rows{row({0})}, {1, 0}},
        RawCompaction{{RawUnit{row({1}), 1}, RawUnit{row({0, 4}), 6}}},
        RawLearned{7, Rows{row({2})}, std::move(learned_reasons)},
        RawLearned{8, Rows{row({2, 4})}, {4, 5}},
        RawEmpty{9, {7, 8}}};
    return test_case;
}

/// @brief A root unit that is a one-row input clause, and so needs no line.
Case alias_case() {
    Case test_case;
    test_case.num_vars = 3;
    test_case.inputs = {Rows{row({0})},                              // 1: [x1 = 1]
                        Rows{row({0, 3}), row({1}), row({2})},       // 2
                        Rows{row({1, 3}), row({2})},                 // 3
                        Rows{row({1, 3}), row({2, 3})},              // 4
                        Rows{row({1}), row({2, 3})}};                // 5
    test_case.records = {RawCompaction{{RawUnit{row({0, 3}), 0}}},
                         RawLearned{5, Rows{row({2})}, {1, 2}},
                         RawLearned{6, Rows{row({2, 3})}, {4, 3}},
                         RawEmpty{7, {5, 6}}};
    return test_case;
}

/// @brief Two epochs, the second one's unit needing the first one's.
Case two_epoch_case() {
    Case test_case;
    test_case.num_vars = 4;
    test_case.inputs = {Rows{row({0})},                              // 1: [x1 = 1]
                        Rows{row({0, 4}), row({2, 4}), row({1})},    // 2
                        Rows{row({0, 4}), row({2}), row({3})},       // 3
                        Rows{row({2}), row({3, 4})},                 // 4
                        Rows{row({1, 4}), row({3})},                 // 5
                        Rows{row({1, 4}), row({3, 4})}};             // 6
    test_case.records = {RawCompaction{{RawUnit{row({0, 4}), 0}}},
                         RawLearned{6, Rows{row({2})}, {2, 3}},
                         RawLearned{7, Rows{row({1})}, {6, 1}},
                         RawCompaction{{RawUnit{row({2, 4}), 6}, RawUnit{row({1, 4}), 7}}},
                         RawEmpty{8, {4, 5}}};
    return test_case;
}

/// @brief A root unit recorded reduced by the one before it.
///
/// The trail eliminates x2 + x3 = 0 and then x1 + x2 = 1, which the record and
/// its line spell x1 + x3 = 1. Clause 3 needs both units, one per row.
Case reduced_row_case() {
    Case test_case;
    test_case.num_vars = 3;
    test_case.inputs = {Rows{row({1, 2, 3})},                // 1: [x2 + x3 = 0]
                        Rows{row({1, 2}), row({0, 1})},      // 2
                        Rows{row({0, 2, 3}), row({1, 2})}};  // 3
    test_case.records = {RawLearned{3, Rows{row({0, 2})}, {0, 1}},
                         RawCompaction{{RawUnit{row({1, 2}), 0}, RawUnit{row({0, 2, 3}), 3}}},
                         RawEmpty{4, {2}}};
    return test_case;
}

} // namespace

TEST_CASE_TEMPLATE("postprocess: an uncompacted proof is trimmed and renumbered", Tag, DenseTag,
                   SparseTag) {
    PostprocessSummary summary;
    std::string text;
    const Case test_case = uncompacted_case();
    const std::string error = run(Tag::value, test_case, summary, text);
    REQUIRE(error == "");
    check_proof_shape(text, test_case.inputs.size());

    // Clause 4 goes unused, the first derivation is unreachable, and every kept
    // clause is deleted right after the line that last names it.
    CHECK(text ==
          "4 0\n"
          "5 2 0 1 2 0\n"
          "1 2 0\n"
          "6 0 5 3 0\n"
          "3 5 0\n");
    CHECK(summary.raw_learned == 3);
    CHECK(summary.kept_learned == 2);
    CHECK(summary.rewrite_lines == 0);
    CHECK(summary.deletion_lines == 3);
}

TEST_CASE_TEMPLATE("postprocess: a compacted reason is rewritten as a line of its own", Tag,
                   DenseTag, SparseTag) {
    PostprocessSummary summary;
    std::string text;
    const Case test_case = compacted_case({2, 3});
    const std::string error = run(Tag::value, test_case, summary, text);
    REQUIRE(error == "");
    check_proof_shape(text, test_case.inputs.size());

    // Clause 3, reduced by the second unit, is proved again for the chain to cite.
    CHECK(summary.raw_learned == 4);
    CHECK(summary.kept_learned == 4);
    CHECK(summary.rewrite_lines == 1);
    CHECK(summary.rewrite_unit_ids == 1);
    CHECK(summary.deletion_lines == 5);
}

TEST_CASE_TEMPLATE("postprocess: a compacted reason at the end of a chain is rewritten", Tag,
                   DenseTag, SparseTag) {
    PostprocessSummary summary;
    std::string text;
    const Case test_case = compacted_case({3, 2});
    const std::string error = run(Tag::value, test_case, summary, text);
    REQUIRE(error == "");
    check_proof_shape(text, test_case.inputs.size());

    // The chain propagates on clause 4 first, then cites the rewrite in clause 3's place.
    CHECK(summary.rewrite_lines == 1);
    CHECK(summary.rewrite_unit_ids == 1);
}

TEST_CASE_TEMPLATE("postprocess: rewriting spans two compactions", Tag, DenseTag, SparseTag) {
    PostprocessSummary summary;
    std::string text;
    const Case test_case = two_epoch_case();
    const std::string error = run(Tag::value, test_case, summary, text);
    REQUIRE(error == "");
    check_proof_shape(text, test_case.inputs.size());

    // Each cited clause is reduced by every unit so far: clauses 2 and 3 by the
    // first epoch's unit, clauses 5 and 6 by the second epoch's.
    CHECK(summary.raw_learned == 3);
    CHECK(summary.kept_learned == 3);
    CHECK(summary.rewrite_lines == 4);
    CHECK(summary.rewrite_unit_ids == 4);
}

TEST_CASE_TEMPLATE("postprocess: a unit reduced by an earlier one maps to its own line", Tag,
                   DenseTag, SparseTag) {
    PostprocessSummary summary;
    std::string text;
    const Case test_case = reduced_row_case();
    const std::string error = run(Tag::value, test_case, summary, text);
    REQUIRE(error == "");
    check_proof_shape(text, test_case.inputs.size());

    // Each pivot maps to its own unit: the learned line for x1 + x3 = 1, clause
    // 1 itself for x2 + x3 = 0. Both reduce clause 3 to no rows at all, so the
    // one rewrite line cites both.
    CHECK(summary.rewrite_lines == 1);
    CHECK(summary.rewrite_unit_ids == 2);
}

TEST_CASE_TEMPLATE("postprocess: a unit from a one-row input clause needs no line", Tag, DenseTag,
                   SparseTag) {
    PostprocessSummary summary;
    std::string text;
    const Case test_case = alias_case();
    const std::string error = run(Tag::value, test_case, summary, text);
    REQUIRE(error == "");
    check_proof_shape(text, test_case.inputs.size());

    // The unit is clause 1 itself, which the line rewriting clause 2 cites.
    CHECK(text.starts_with("6 2 3 0 1 2 0\n"));
    CHECK(summary.rewrite_lines == 1);
    CHECK(summary.rewrite_unit_ids == 1);
}

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

#ifdef XORCLE_HAS_DEATH_TESTS
TEST_CASE("postprocess: a raw proof breaking the solver's invariants asserts") {
    // Only the solver's own raw proof reaches post-processing, so these are
    // internal invariants rather than input errors.
    const auto dies_on = [](Case test_case) {
        return dies_in_child([&] {
            RawRecordVectorSource source(test_case.records);
            std::ostringstream out;
            postprocess_raw_proof(source, test_case.num_vars, test_case.inputs.size(),
                                  EquationRepr::dense, out, test_case.inputs);
        });
    };

    SUBCASE("a root unit naming no line") {
        CHECK(dies_on(Case{3, {Rows{row({0})}},
                           {RawCompaction{{RawUnit{row({0, 3}), 5}}}, RawEmpty{1, {0}}}}));
    }

    SUBCASE("a root unit not reduced by the one before it") {
        CHECK(dies_on(Case{3,
                           {Rows{row({0})}, Rows{row({0, 1, 3})}},
                           {RawCompaction{{RawUnit{row({0, 3}), 0}, RawUnit{row({0, 1}), 1}}},
                            RawEmpty{2, {0}}}}));
    }

    SUBCASE("a root unit holding an earlier compaction's pivot") {
        // x1 + x2 = 0 leads with x1, no pivot, but holds x2, which the first
        // compaction eliminated.
        CHECK(dies_on(Case{3,
                           {Rows{row({1, 3})}, Rows{row({0, 1})}},
                           {RawCompaction{{RawUnit{row({1, 3}), 0}}},
                            RawCompaction{{RawUnit{row({0, 1}), 1}}}, RawEmpty{2, {0}}}}));
    }

    SUBCASE("a reason the proof has deleted") {
        CHECK(dies_on(Case{2,
                           {Rows{row({0})}, Rows{row({0, 2})}},
                           {RawDeleted{{1}}, RawEmpty{2, {0, 1}}}}));
    }
}
#endif

} // namespace xorcle
