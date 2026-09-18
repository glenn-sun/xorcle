#include <doctest/doctest.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "algebra/equation_repr.h"
#include "algebra/row_echelon_matrix.h"
#include "algebra_test_support.h"
#include "core/clause_id.h"
#include "io/proof_raw.h"
#include "io/proof_raw_file.h"

namespace xorcle {

namespace {

/// @brief A raw proof path in the temp directory, unique to one test case.
std::filesystem::path raw_path(EquationRepr repr, std::string_view name) {
    const std::string suffix = repr == EquationRepr::dense ? "_dense.raw" : "_sparse.raw";
    return std::filesystem::temp_directory_path() /
           ("xorcle_raw_proof_test_" + std::string{name} + suffix);
}

/// @brief Every record of a file, read back through the file source.
std::vector<RawRecord> read_all(RawProofFileSource& source) {
    std::vector<RawRecord> records;
    RawRecord record;
    while (source.next(record)) {
        records.push_back(record);
    }
    return records;
}

/// @brief A clause with the listed rows, over `num_vars` variables.
Clause make_clause(EquationRepr repr, std::size_t num_vars,
                   const std::vector<std::vector<std::size_t>>& rows) {
    Clause clause(repr, num_vars);
    for (const std::vector<std::size_t>& bits : rows) {
        Equation row = Equation::with_num_bits(repr, num_vars + 1);
        row.assign_bits(bits);
        clause.add_row(row);
    }
    return clause;
}

} // namespace

TEST_CASE_TEMPLATE("raw proof file: every record round-trips", Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    const std::filesystem::path path = raw_path(repr, "records");

    // Two active variables standing for original 1 and 3, so the active constant
    // bit 2 lifts to original bit 5.
    const std::vector<std::size_t> active_var_to_original{1, 3};

    {
        RawProofWriter writer(path, 5, 4);
        CHECK(writer.num_vars() == 5);
        CHECK(writer.num_input_clauses() == 4);

        writer.write_learned(ClauseId{4}, make_clause(repr, 2, {{0, 2}, {1}}),
                             active_var_to_original, std::vector<ClauseId>{0, 2, 3});
        // Rows already over the original variables are written as they are.
        const std::vector<RawRow> lifted{RawRow{0, 4}};
        writer.write_learned(ClauseId{5}, lifted, std::vector<ClauseId>{4, 1});
        writer.write_compaction(std::vector<RawUnit>{RawUnit{RawRow{0, 5}, ClauseId{1}},
                                                     RawUnit{RawRow{2}, ClauseId{3}}});
        writer.write_deleted(std::vector<ClauseId>{2, 7});
        // An empty clause whose reasons are empty: the shortest record there is.
        writer.write_empty(ClauseId{9}, std::vector<ClauseId>{});
        writer.finalize(true);
    }

    RawProofFileSource source(path);
    CHECK(source.num_vars() == 5);
    CHECK(source.num_input_clauses() == 4);

    const std::vector<RawRecord> records = read_all(source);
    REQUIRE(records.size() == 5);

    const RawLearned& learned = std::get<RawLearned>(records[0]);
    CHECK(learned.id == 4);
    // Active bits {0, 2} lift to {1, 5}, the second being the constant bit.
    CHECK(learned.rows == std::vector<RawRow>{RawRow{1, 5}, RawRow{3}});
    CHECK(learned.reasons == std::vector<ClauseId>{0, 2, 3});

    const RawLearned& unit = std::get<RawLearned>(records[1]);
    CHECK(unit.id == 5);
    CHECK(unit.rows == std::vector<RawRow>{RawRow{0, 4}});
    CHECK(unit.reasons == std::vector<ClauseId>{4, 1});

    const RawCompaction& compaction = std::get<RawCompaction>(records[2]);
    REQUIRE(compaction.units.size() == 2);
    CHECK(compaction.units[0].row == RawRow{0, 5});
    CHECK(compaction.units[0].id == 1);
    CHECK(compaction.units[1].row == RawRow{2});
    CHECK(compaction.units[1].id == 3);

    CHECK(std::get<RawDeleted>(records[3]).ids == std::vector<ClauseId>{2, 7});

    const RawEmpty& empty = std::get<RawEmpty>(records[4]);
    CHECK(empty.id == 9);
    CHECK(empty.reasons.empty());

    // Rewinding replays the same records from the first one.
    source.rewind();
    CHECK(read_all(source).size() == 5);

    std::filesystem::remove(path);
}

TEST_CASE("raw proof file: an empty proof holds only its header") {
    const std::filesystem::path path = raw_path(EquationRepr::dense, "header_only");
    {
        RawProofWriter writer(path, 1000000, 0);
        writer.finalize(true);
    }

    RawProofFileSource source(path);
    CHECK(source.num_vars() == 1000000);
    CHECK(source.num_input_clauses() == 0);
    RawRecord record;
    CHECK_FALSE(source.next(record));

    std::filesystem::remove(path);
}

TEST_CASE("raw proof file: finalize(false) leaves nothing behind") {
    const std::filesystem::path path = raw_path(EquationRepr::dense, "discarded");
    std::filesystem::remove(path);
    {
        RawProofWriter writer(path, 2, 1);
        writer.write_empty(ClauseId{1}, std::vector<ClauseId>{0});
        writer.finalize(false);
    }
    CHECK_FALSE(std::filesystem::exists(path));

    // Nor does an abandoned writer, whose destructor removes the temp file.
    {
        RawProofWriter writer(path, 2, 1);
        writer.write_empty(ClauseId{1}, std::vector<ClauseId>{0});
    }
    CHECK_FALSE(std::filesystem::exists(path));
}

TEST_CASE("raw proof file: a truncated or foreign file is refused") {
    const std::filesystem::path path = raw_path(EquationRepr::dense, "truncated");
    {
        RawProofWriter writer(path, 3, 1);
        writer.write_deleted(std::vector<ClauseId>{1, 2, 3});
        writer.finalize(true);
    }

    // Cut the file one byte short: the header still parses, the record does not.
    const std::size_t size = std::filesystem::file_size(path);
    std::filesystem::resize_file(path, size - 1);
    RawProofFileSource source(path);
    RawRecord record;
    CHECK_THROWS_AS(source.next(record), RawProofError);

    std::ofstream foreign{path, std::ios::out | std::ios::binary | std::ios::trunc};
    foreign << "not a raw proof at all";
    foreign.close();
    CHECK_THROWS_AS(RawProofFileSource{path}, RawProofError);

    std::filesystem::remove(path);
    CHECK_THROWS_AS(RawProofFileSource{path}, RawProofError);
}

TEST_CASE_TEMPLATE("raw proof file: lifting a row renumbers and reorders it", Tag, DenseTag,
                   SparseTag) {
    constexpr EquationRepr repr = Tag::value;

    // A map that is not increasing, so the lifted bits need sorting.
    const std::vector<std::size_t> active_var_to_original{4, 0, 2};
    const Equation row = make_equation_with(repr, 3, {0, 1, 3});
    CHECK(lift_row(row, active_var_to_original, 9) == RawRow{0, 4, 9});
}

} // namespace xorcle
