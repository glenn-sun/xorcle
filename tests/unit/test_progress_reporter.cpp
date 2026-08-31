/// @file test_progress_reporter.cpp
/// @brief The progress report format.
///
/// The shape checks here are the point. Column
/// names carry spaces, so only a value line splits on whitespace.

#include <doctest/doctest.h>

#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include "util/progress_reporter.h"

namespace xorcle {

namespace {

constexpr const char* expected_header =
    "c  seconds   conflicts   propagations  restarts   decisions   "
    "trail level   trail rank   remaining vars   deletion passes   deleted   demoted"
    "   promoted";

ProgressSnapshot sample_snapshot() {
    return ProgressSnapshot{.time_s = 1.5,
                            .num_conflicts = 1024,
                            .num_propagations = 987654,
                            .num_restarts = 3,
                            .decisions = 2048,
                            .trail_level = 17,
                            .trail_rank = 41,
                            .remaining_vars = 128,
                            .num_deletion_passes = 5,
                            .num_deleted_clauses = 640,
                            .num_demoted_clauses = 512,
                            .num_promoted_clauses = 96};
}

std::vector<std::string> lines_of(const std::string& text) {
    std::vector<std::string> lines;
    std::istringstream stream(text);
    for (std::string line; std::getline(stream, line);) {
        lines.push_back(line);
    }
    return lines;
}

/// @brief The whitespace-separated fields of a value line, with the leading `c`
/// gone.
std::vector<std::string> values_of(const std::string& line) {
    std::istringstream stream(line);
    std::vector<std::string> fields;
    for (std::string field; stream >> field;) {
        fields.push_back(field);
    }
    REQUIRE(!fields.empty());
    REQUIRE(fields.front() == "c");
    fields.erase(fields.begin());
    return fields;
}

} // namespace

TEST_CASE("the header precedes the first line and appears once") {
    std::ostringstream out;
    ConsoleProgressReporter reporter(out);
    reporter.report(sample_snapshot());
    reporter.report(sample_snapshot());

    const std::vector<std::string> lines = lines_of(out.str());
    REQUIRE(lines.size() == 3);
    CHECK(lines[0] == expected_header);
    CHECK(lines[1] == lines[2]);
}

TEST_CASE("every column carries its snapshot value") {
    std::ostringstream out;
    ConsoleProgressReporter reporter(out);
    reporter.report(sample_snapshot());

    const std::vector<std::string> lines = lines_of(out.str());
    REQUIRE(lines.size() == 2);
    CHECK(values_of(lines[1]) == std::vector<std::string>{"1.50", "1024", "987654", "3",
                                                          "2048", "17", "41", "128",
                                                          "5", "640", "512", "96"});
}

TEST_CASE("no progress line has the shape the stats parser reads") {
    std::ostringstream out;
    ConsoleProgressReporter reporter(out);
    reporter.report(sample_snapshot());

    const std::regex key_value(R"(^c\s+(Conflicts|Decisions|Propagations|Time)\s*:.*$)",
                               std::regex::icase);
    const std::regex status(R"(^s\s+.+$)", std::regex::icase);
    for (const std::string& line : lines_of(out.str())) {
        CHECK(!std::regex_match(line, key_value));
        CHECK(!std::regex_match(line, status));
    }
}

TEST_CASE("columns stay aligned as values widen") {
    std::ostringstream out;
    ConsoleProgressReporter reporter(out);
    reporter.report(ProgressSnapshot{});
    reporter.report(sample_snapshot());

    const std::vector<std::string> lines = lines_of(out.str());
    REQUIRE(lines.size() == 3);
    CHECK(lines[0].size() == lines[1].size());
    CHECK(lines[1].size() == lines[2].size());
}

} // namespace xorcle
