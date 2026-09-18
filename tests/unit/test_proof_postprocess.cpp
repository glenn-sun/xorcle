#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "algebra/equation.h"
#include "algebra/equation_repr.h"
#include "algebra/row_echelon_matrix.h"
#include "algebra_test_support.h"
#include "io/proof_postprocess.h"
#include "io/proof_raw.h"

namespace xorcle {

namespace {

using Rows = std::vector<RawRow>;

RawRow row(std::initializer_list<std::size_t> bits) { return RawRow(bits); }

/// @brief The LRUP(⊕) checker of `tools/check_lrup_parity.py`, over `RowSpace`.
///
/// The Python basis takes a row's highest set bit as its pivot and this one its
/// lowest, but both reduce fully, so they agree on membership and on which
/// residues are equal -- all the checker ever asks.
class LrupChecker {
public:
    LrupChecker(EquationRepr repr, std::size_t num_vars, const std::vector<Rows>& inputs)
        : repr_(repr), num_vars_(num_vars) {
        for (std::size_t index = 0; index < inputs.size(); ++index) {
            clauses_.emplace(index + 1, inputs[index]);
        }
    }

    /// @return An error message, empty when the proof checks out.
    std::string check(const std::string& text) {
        std::istringstream text_stream(text);
        std::string line;
        while (std::getline(text_stream, line)) {
            std::vector<std::string> tokens;
            std::istringstream line_stream(line);
            for (std::string token; line_stream >> token;) {
                tokens.push_back(token);
            }
            if (tokens.empty()) {
                continue;
            }
            const std::string error = check_line(tokens);
            if (!error.empty()) {
                return error + " in '" + line + "'";
            }
        }
        return "";
    }

private:
    enum class Status { conflict, deduced, none };

    std::string check_line(const std::vector<std::string>& tokens) {
        const std::size_t zeros =
            static_cast<std::size_t>(std::count(tokens.begin(), tokens.end(), "0"));
        if (zeros == 1) {
            for (std::size_t i = 0; i + 1 < tokens.size(); ++i) {
                clauses_.erase(std::stoul(tokens[i]));
            }
            return "";
        }
        if (zeros != 2) {
            return "a proof line must contain exactly one or two zeros";
        }
        const std::size_t first =
            static_cast<std::size_t>(std::find(tokens.begin(), tokens.end(), "0") -
                                     tokens.begin());
        const std::size_t second = static_cast<std::size_t>(
            std::find(tokens.begin() + static_cast<std::ptrdiff_t>(first) + 1, tokens.end(), "0") -
            tokens.begin());
        if (first == 0) {
            return "missing learned clause id";
        }

        const std::size_t new_id = std::stoul(tokens[0]);
        if (clauses_.count(new_id) != 0) {
            return "duplicate clause id " + tokens[0];
        }
        Rows rows;
        for (std::size_t i = 1; i < first; ++i) {
            rows.push_back(parse_token(tokens[i]));
        }

        RowSpace basis(repr_, num_vars_);
        for (const RawRow& clause_row : rows) {
            basis.add_row(equation(clause_row));
        }
        bool verified = false;
        for (std::size_t i = first + 1; i < second; ++i) {
            const auto reason = clauses_.find(std::stoul(tokens[i]));
            if (reason == clauses_.end()) {
                return "missing reason clause " + tokens[i];
            }
            const Status status = propagate(reason->second, basis);
            if (status == Status::conflict) {
                verified = true;
                break;
            }
            if (status == Status::none) {
                return "reason clause " + tokens[i] + " did not propagate";
            }
        }
        if (!verified) {
            return "no conflict derived for " + tokens[0];
        }
        clauses_[new_id] = std::move(rows);
        return "";
    }

    Status propagate(const Rows& rows, RowSpace& basis) const {
        std::vector<RawRow> residues;
        for (const RawRow& clause_row : rows) {
            Equation reduced = equation(clause_row);
            basis.reduce(reduced);
            if (reduced.leading_bit()) {
                residues.push_back(reduced.bits());
            }
        }
        if (residues.empty()) {
            return Status::conflict;
        }
        for (const RawRow& residue : residues) {
            if (residue != residues[0]) {
                return Status::none;
            }
        }
        const RawRow deduced = negate(residues[0]);
        if (contains(basis, residues[0]) || contains(basis, deduced)) {
            return Status::none;
        }
        basis.add_row(equation(deduced));
        return Status::deduced;
    }

    bool contains(const RowSpace& basis, const RawRow& bits) const {
        Equation reduced = equation(bits);
        basis.reduce(reduced);
        return !reduced.leading_bit().has_value();
    }

    Equation equation(const RawRow& bits) const {
        Equation out = make_equation(repr_, num_vars_);
        out.assign_bits(bits);
        return out;
    }

    RawRow negate(const RawRow& bits) const {
        RawRow out = bits;
        if (!out.empty() && out.back() == num_vars_) {
            out.pop_back();
        } else {
            out.push_back(num_vars_);
        }
        return out;
    }

    RawRow parse_token(const std::string& token) const {
        RawRow bits;
        std::size_t position = 0;
        const bool constant = !token.empty() && token[0] == '-';
        if (constant) {
            position = 1;
        }
        while (position < token.size()) {
            const std::size_t plus = token.find('+', position);
            const std::size_t length = plus == std::string::npos ? plus : plus - position;
            bits.push_back(std::stoul(token.substr(position, length)) - 1);
            if (plus == std::string::npos) {
                break;
            }
            position = plus + 1;
        }
        std::sort(bits.begin(), bits.end());
        if (constant) {
            bits.push_back(num_vars_);
        }
        return bits;
    }

    EquationRepr repr_;
    std::size_t num_vars_;
    std::unordered_map<std::size_t, Rows> clauses_;
};

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
    const std::string error = run(Tag::value, uncompacted_case(), summary, text);
    REQUIRE(error == "");

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
    const std::string error = run(Tag::value, compacted_case({2, 3}), summary, text);
    REQUIRE(error == "");

    // Clause 3 reduced by unit line 7 is proved as line 8, which line 9 cites
    // in its place; clause 3 and line 7 go right after it.
    CHECK(text ==
          "7 1 0 2 1 0\n"
          "1 2 0\n"
          "8 -4 3 0 7 3 0\n"
          "3 7 0\n"
          "9 3 0 8 4 0\n"
          "4 8 0\n"
          "10 -3 0 5 6 0\n"
          "5 6 0\n"
          "11 0 9 10 0\n"
          "9 10 0\n");
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
    const std::string error = run(Tag::value, compacted_case({3, 2}), summary, text);
    REQUIRE(error == "");

    // The chain propagates on clause 4 first, then cites line 8 in clause 3's place.
    CHECK(text ==
          "7 1 0 2 1 0\n"
          "1 2 0\n"
          "8 -4 3 0 7 3 0\n"
          "3 7 0\n"
          "9 3 0 4 8 0\n"
          "4 8 0\n"
          "10 -3 0 5 6 0\n"
          "5 6 0\n"
          "11 0 9 10 0\n"
          "9 10 0\n");
    CHECK(summary.rewrite_lines == 1);
    CHECK(summary.rewrite_unit_ids == 1);
}

TEST_CASE_TEMPLATE("postprocess: rewriting spans two compactions", Tag, DenseTag, SparseTag) {
    PostprocessSummary summary;
    std::string text;
    const std::string error = run(Tag::value, two_epoch_case(), summary, text);
    REQUIRE(error == "");

    // Each cited clause is reduced by every unit so far: clauses 2 and 3 by
    // line 1, clauses 5 and 6 by line 10.
    CHECK(text ==
          "7 3 4 0 1 3 0\n"
          "3 0\n"
          "8 3 0 7 4 0\n"
          "4 7 0\n"
          "9 -3 2 0 1 2 0\n"
          "1 2 0\n"
          "10 2 0 8 9 0\n"
          "8 9 0\n"
          "11 4 0 10 5 0\n"
          "5 0\n"
          "12 -4 0 10 6 0\n"
          "6 10 0\n"
          "13 0 11 12 0\n"
          "11 12 0\n");
    CHECK(summary.raw_learned == 3);
    CHECK(summary.kept_learned == 3);
    CHECK(summary.rewrite_lines == 4);
    CHECK(summary.rewrite_unit_ids == 4);
}

TEST_CASE_TEMPLATE("postprocess: a unit reduced by an earlier one maps to its own line", Tag,
                   DenseTag, SparseTag) {
    PostprocessSummary summary;
    std::string text;
    const std::string error = run(Tag::value, reduced_row_case(), summary, text);
    REQUIRE(error == "");

    // Each pivot maps to its own unit: line 4 for x1 + x3 = 1, clause 1 itself
    // for x2 + x3 = 0. Both reduce clause 3 to no rows at all.
    CHECK(text ==
          "4 1+3 0 1 2 0\n"
          "2 0\n"
          "5 0 4 1 3 0\n"
          "1 3 4 0\n"
          "6 0 5 0\n"
          "5 0\n");
    CHECK(summary.rewrite_lines == 1);
    CHECK(summary.rewrite_unit_ids == 2);
}

TEST_CASE_TEMPLATE("postprocess: a unit from a one-row input clause needs no line", Tag, DenseTag,
                   SparseTag) {
    PostprocessSummary summary;
    std::string text;
    const std::string error = run(Tag::value, alias_case(), summary, text);
    REQUIRE(error == "");

    // The unit is clause 1 itself, which the line rewriting clause 2 cites.
    CHECK(text ==
          "6 2 3 0 1 2 0\n"
          "1 2 0\n"
          "7 3 0 6 3 0\n"
          "3 6 0\n"
          "8 -3 0 5 4 0\n"
          "4 5 0\n"
          "9 0 7 8 0\n"
          "7 8 0\n");
    CHECK(summary.rewrite_lines == 1);
    CHECK(summary.rewrite_unit_ids == 1);
}

TEST_CASE_TEMPLATE("postprocess: a raw proof that cannot be turned into a proof throws", Tag,
                   DenseTag, SparseTag) {
    std::ostringstream out;

    SUBCASE("no empty clause") {
        Case test_case = uncompacted_case();
        test_case.records.pop_back();
        RawRecordVectorSource source(test_case.records);
        CHECK_THROWS_AS(postprocess_raw_proof(source, test_case.num_vars, test_case.inputs.size(),
                                              Tag::value, out, test_case.inputs),
                        std::runtime_error);
    }

    SUBCASE("a root unit naming no line") {
        Case test_case;
        test_case.num_vars = 3;
        test_case.inputs = {Rows{row({0})}};
        test_case.records = {RawCompaction{{RawUnit{row({0, 3}), 5}}}, RawEmpty{1, {0}}};
        RawRecordVectorSource source(test_case.records);
        CHECK_THROWS_AS(postprocess_raw_proof(source, test_case.num_vars, test_case.inputs.size(),
                                              Tag::value, out, test_case.inputs),
                        std::runtime_error);
    }

    SUBCASE("a root unit not reduced by the one before it") {
        Case test_case;
        test_case.num_vars = 3;
        test_case.inputs = {Rows{row({0})}, Rows{row({0, 1, 3})}};
        test_case.records = {RawCompaction{{RawUnit{row({0, 3}), 0}, RawUnit{row({0, 1}), 1}}},
                             RawEmpty{2, {0}}};
        RawRecordVectorSource source(test_case.records);
        CHECK_THROWS_AS(postprocess_raw_proof(source, test_case.num_vars, test_case.inputs.size(),
                                              Tag::value, out, test_case.inputs),
                        std::runtime_error);
    }

    SUBCASE("a root unit holding an earlier compaction's pivot") {
        // x1 + x2 = 0 leads with x1, no pivot, but holds x2, which the first
        // compaction eliminated.
        Case test_case;
        test_case.num_vars = 3;
        test_case.inputs = {Rows{row({1, 3})}, Rows{row({0, 1})}};
        test_case.records = {RawCompaction{{RawUnit{row({1, 3}), 0}}},
                             RawCompaction{{RawUnit{row({0, 1}), 1}}}, RawEmpty{2, {0}}};
        RawRecordVectorSource source(test_case.records);
        CHECK_THROWS_WITH_AS(
            postprocess_raw_proof(source, test_case.num_vars, test_case.inputs.size(), Tag::value,
                                  out, test_case.inputs),
            "a root unit holds a variable an earlier compaction eliminated", std::runtime_error);
    }
}

} // namespace xorcle
