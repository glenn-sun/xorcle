/// @file test_progress_reporter.cpp
/// @brief The progress report format.
///
/// The columnar shape is what keeps a progress line from being read as the run's
/// totals, so the shape checks here are the point. Column names carry spaces, so
/// only a value line splits on whitespace.

#include <doctest/doctest.h>

#include <algorithm>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include "util/progress_reporter.h"

namespace xorcle {

namespace {

constexpr const char* expected_default_header =
    "c   time  conflicts  confl/s    props  clauses  avg level  avg trail  vars left";

ProgressSnapshot sample_snapshot() {
    return ProgressSnapshot{.time_s = 1.5,
                            .num_conflicts = 1024,
                            .conflicts_per_second = 682.5,
                            .num_propagations = 987654,
                            .num_clauses = 4096,
                            .average_decision_level = 17.25,
                            .average_trail_length = 41.5,
                            .remaining_vars = 128,
                            .decisions = 2048,
                            .decision_level = 17,
                            .trail_length = 41,
                            .num_restarts = 3,
                            .num_reused_levels = 7,
                            .num_deleted_clauses = 640,
                            .num_attempt_props = 31415,
                            .num_watch_updates = 2718};
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

TEST_CASE("progress: the header precedes the first line") {
    std::ostringstream out;
    ConsoleProgressReporter reporter(out);
    reporter.report(sample_snapshot());
    reporter.report(sample_snapshot());

    const std::vector<std::string> lines = lines_of(out.str());
    REQUIRE(lines.size() == 3);
    CHECK(lines[0] == expected_default_header);
    CHECK(lines[1] == lines[2]);
}

TEST_CASE("progress: the header repeats so it stays on screen") {
    std::ostringstream out;
    ConsoleProgressReporter reporter(out);
    // One more line than the interval, so exactly two headers are due.
    for (std::size_t i = 0; i <= 25; ++i) {
        reporter.report(sample_snapshot());
    }

    const std::vector<std::string> lines = lines_of(out.str());
    const std::size_t headers =
        static_cast<std::size_t>(std::count(lines.begin(), lines.end(), expected_default_header));
    CHECK(headers == 2);
    CHECK(lines.size() == 26 + headers);
}

TEST_CASE("progress: the default columns carry their snapshot values") {
    std::ostringstream out;
    ConsoleProgressReporter reporter(out);
    reporter.report(sample_snapshot());

    const std::vector<std::string> lines = lines_of(out.str());
    REQUIRE(lines.size() == 2);
    CHECK(values_of(lines[1]) == std::vector<std::string>{"1.50", "1024", "682.5", "987654",
                                                          "4096", "17.2", "41.5", "128"});
}

TEST_CASE("progress: a group adds its columns to the default ones") {
    std::ostringstream out;
    ConsoleProgressReporter reporter(out, progress_group::defaults | progress_group::deletion);
    reporter.report(sample_snapshot());

    const std::vector<std::string> lines = lines_of(out.str());
    REQUIRE(lines.size() == 2);
    CHECK(values_of(lines[1]) == std::vector<std::string>{"1.50", "1024", "682.5", "987654",
                                                          "4096", "17.2", "41.5", "128",
                                                          "640"});
}

TEST_CASE("progress: --report names map onto the groups") {
    CHECK(parse_progress_groups("default") == progress_group::defaults);
    CHECK(parse_progress_groups("all") == progress_group::all);
    // The default columns are what makes a line readable, so they are never off.
    CHECK(parse_progress_groups("deletion") ==
          (progress_group::defaults | progress_group::deletion));
    CHECK(parse_progress_groups("search,deletion") ==
          (progress_group::defaults | progress_group::search | progress_group::deletion));

    CHECK(!parse_progress_groups("nope").has_value());
    CHECK(!parse_progress_groups("").has_value());
    CHECK(!parse_progress_groups("search,").has_value());
}

TEST_CASE("progress: no progress line has the shape of an end-of-run statistic") {
    std::ostringstream out;
    ConsoleProgressReporter reporter(out, progress_group::all);
    reporter.report(sample_snapshot());

    const std::regex key_value(R"(^c\s+[A-Za-z][A-Za-z ]*\s*:.*$)");
    const std::regex status(R"(^s\s+.+$)", std::regex::icase);
    for (const std::string& line : lines_of(out.str())) {
        CHECK(!std::regex_match(line, key_value));
        CHECK(!std::regex_match(line, status));
    }
}

TEST_CASE("progress: columns stay aligned as values widen") {
    std::ostringstream out;
    ConsoleProgressReporter reporter(out, progress_group::all);
    reporter.report(ProgressSnapshot{});
    reporter.report(sample_snapshot());

    const std::vector<std::string> lines = lines_of(out.str());
    REQUIRE(lines.size() == 3);
    CHECK(lines[0].size() == lines[1].size());
    CHECK(lines[1].size() == lines[2].size());
}

} // namespace xorcle
