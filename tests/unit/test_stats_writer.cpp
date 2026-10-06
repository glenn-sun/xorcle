/// @file test_stats_writer.cpp
/// @brief The console and CSV stats formats.
///
/// Both are generated from `util/stats_fields.inc`, so the checks here are about
/// what that generation must guarantee: the two formats carry the same fields,
/// a section prints only when the run had something to say, and the CSV schema
/// does not depend on the configuration. The console block is read by people, so
/// its shape is pinned byte-exactly.

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "util/index_set.h"
#include "util/stats.h"
#include "util/stats_writer.h"
#include "io_test_support.h"

namespace xorcle {

namespace {

/// @brief Capture what a console writer writes to stdout.
std::string console_output(const RunStats& stats) {
    std::ostringstream captured;
    std::streambuf* saved = std::cout.rdbuf(captured.rdbuf());
    ConsoleStatsWriter writer;
    writer.log(stats);
    std::cout.rdbuf(saved);
    return captured.str();
}

/// @brief The options a run records, as `option_settings()` would give them.
///
/// Held here rather than taken from `cli/options.h` so that these tests pin the
/// writers alone; only `restart`, `minimize` and `deletion` steer the output.
std::vector<ConfigEntry> sample_config() {
    return {
        ConfigEntry{"seed", "12345", false},
        ConfigEntry{"decisions", "csids", true},
        ConfigEntry{"restart", "none", true},
        ConfigEntry{"minimize", "true", true},
        ConfigEntry{"deletion", "false", false},
    };
}

/// @brief A `RunStats` with every counter set, for format checks.
RunStats sample_stats() {
    RunStats stats;
    stats.file = "benchmarks/generated/random_kxnf/random_kxnf_n12_k2_s1.xnf";
    stats.config = sample_config();
    stats.result = SolveResult::unsat;

    stats.num_vars = 12;
    stats.active_vars = 9;
    stats.start_clauses = 30;

    stats.num_conflicts = 17;
    stats.decisions = 40;
    stats.fallback_decisions = 3;
    stats.num_propagations = 200;
    stats.num_attempt_props = 900;
    stats.num_watch_updates = 83;
    stats.num_compactions = 4;

    stats.num_restarts = 2;
    stats.num_reused_levels = 0;
    stats.num_canceled_restarts = 5;
    stats.fast_lbd = 3.25;
    stats.slow_lbd = 4.5;

    stats.learned_clauses = 17;
    stats.clauses_in_database = 41;
    stats.clauses_allocated = 47;
    stats.average_learned_lbd = 2.5;
    stats.learned_equation_bits = 630;

    stats.num_minimized_clauses = 11;
    stats.minimize_width_before = 146;
    stats.minimize_width_after = 90;

    stats.num_deletion_passes = 4;
    stats.num_deleted_clauses = 6;

    stats.parse_time_s = 0.002;
    stats.solve_time_s = 0.0128;
    return stats;
}

/// @brief A scratch path that removes itself.
class TempFile {
public:
    TempFile()
        : path_(scratch_path("stats_" + std::to_string(next_index_++) + ".csv")) {
        std::filesystem::remove(path_);
    }
    ~TempFile() { std::filesystem::remove(path_); }

    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;

    const std::string& path() const { return path_string_; }

private:
    static inline std::size_t next_index_ = 0;
    std::filesystem::path path_;
    std::string path_string_ = path_.string();
};

std::string read_file(const std::string& path) {
    std::ifstream in(path);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

/// @brief The header's fields, in order.
std::vector<std::string> header_of(const std::string& csv) {
    std::istringstream lines(csv);
    std::string header;
    std::getline(lines, header);

    std::vector<std::string> fields;
    std::istringstream stream(header);
    for (std::string field; std::getline(stream, field, ',');) {
        fields.push_back(field);
    }
    return fields;
}

/// @brief The one data row's fields, in order.
std::vector<std::string> row_of(const std::string& csv) {
    std::istringstream lines(csv);
    std::string discarded;
    std::string row;
    std::getline(lines, discarded);
    std::getline(lines, row);

    std::vector<std::string> fields;
    std::istringstream stream(row);
    for (std::string field; std::getline(stream, field, ',');) {
        fields.push_back(field);
    }
    return fields;
}

} // namespace

TEST_CASE("stats: to_string(SolveResult) uses the CSV spellings") {
    CHECK(to_string(SolveResult::sat) == "SAT");
    CHECK(to_string(SolveResult::unsat) == "UNSAT");
}

TEST_CASE("stats: config_value reads the recorded configuration") {
    const RunStats stats = sample_stats();
    CHECK(config_value(stats, "decisions") == "csids");
    CHECK(config_value(stats, "deletion") == "false");
    CHECK(config_value(stats, "absent") == "");
}

TEST_CASE("stats: a section reports only when the configuration says it ran") {
    RunStats stats = sample_stats();

    // Always, whatever the run did.
    CHECK(stats.reports(StatsSection::instance));
    CHECK(stats.reports(StatsSection::search));
    CHECK(stats.reports(StatsSection::clauses));
    CHECK(stats.reports(StatsSection::timing));

    // Steered by the recorded options, so a block can never claim a feature ran
    // that the same row says was off.
    CHECK(stats.reports(StatsSection::minimize));
    CHECK(!stats.reports(StatsSection::deletion));
    CHECK(!stats.reports(StatsSection::restart));
    CHECK(!stats.reports(StatsSection::implication_graph));

    stats.config = {ConfigEntry{"restart", "lbd", false}, ConfigEntry{"minimize", "false", false},
                    ConfigEntry{"deletion", "true", true},
                    ConfigEntry{"implication_graph", "true", false}};
    CHECK(stats.reports(StatsSection::restart));
    CHECK(!stats.reports(StatsSection::minimize));
    CHECK(stats.reports(StatsSection::deletion));
    CHECK(stats.reports(StatsSection::implication_graph));

    // The proof section waits on post-processing having actually run.
    CHECK(!stats.reports(StatsSection::proof));
    stats.proof_postprocess_time_s = 0.5;
    CHECK(stats.reports(StatsSection::proof));
}

TEST_CASE("ConsoleStatsWriter: the UNSAT format") {
    const std::string expected =
        "c File: benchmarks/generated/random_kxnf/random_kxnf_n12_k2_s1.xnf\n"
        "c Variables: 12\n"
        "c Active variables: 9\n"
        "c Start clauses: 30\n"
        "c Configuration (3 further options at their defaults):\n"
        "c   seed = 12345\n"
        "c   deletion = false\n"
        "c Conflicts: 17\n"
        "c Decisions: 40\n"
        "c Fallback decisions: 3\n"
        "c Propagations: 200\n"
        "c Attempted propagations: 900\n"
        "c Watch updates: 83\n"
        "c Compactions: 4\n"
        "c Compactions that renumbered: 0\n"
        "c Variables dropped: 0\n"
        "c Clauses rewritten by a compaction: 0\n"
        "c Peak column-to-clause entries: 0\n"
        "c Learned clauses: 17\n"
        "c Clauses in database: 41\n"
        "c Clauses allocated: 47\n"
        "c Average learned LBD: 2.500\n"
        "c Learned equation bits: 630\n"
        "c Learned width-1 clauses: 0\n"
        "c Learned width-2 clauses: 0\n"
        "c Subsumption replacements of width at most 2: 0\n"
        "c Compaction narrowings to width at most 2: 0\n"
        "c Minimized clauses: 11\n"
        "c Width before minimizing: 146\n"
        "c Width after minimizing: 90\n"
        "c Parse time (s): 0.002\n"
        "c Solve time (s): 0.013\n"
        "c Decision time (s), also in the solve time: 0.000\n"
        "c Propagations per conflict: 11.765\n"
        "c Attempts per propagation, watch update or conflict: 3.000\n"
        "c Fallback decision rate: 7.50%\n"
        "c Conflicts per second: 1328.1\n"
        "c Propagations per second: 15625.0\n"
        "s UNSATISFIABLE\n";
    CHECK(console_output(sample_stats()) == expected);
}

TEST_CASE("ConsoleStatsWriter: a run at every default says so in one line") {
    RunStats stats = sample_stats();
    for (ConfigEntry& entry : stats.config) {
        entry.is_default = true;
    }
    CHECK(console_output(stats).find("c Configuration: all 5 options at their defaults\n") !=
          std::string::npos);
}

TEST_CASE("ConsoleStatsWriter: the deletion lines appear only when it ran") {
    RunStats stats = sample_stats();
    CHECK(console_output(stats).find("c Deletion passes:") == std::string::npos);

    stats.config = {ConfigEntry{"deletion", "true", true}};
    const std::string output = console_output(stats);
    CHECK(output.find("c Deletion passes: 4\n"
                      "c Deleted clauses: 6\n") != std::string::npos);
}

TEST_CASE("ConsoleStatsWriter: the restart lines appear only under a schedule") {
    RunStats stats = sample_stats();
    CHECK(console_output(stats).find("c Restarts:") == std::string::npos);

    stats.config = {ConfigEntry{"restart", "lbd", false}};
    const std::string output = console_output(stats);
    CHECK(output.find("c Restarts: 2\n"
                      "c Reused levels: 0\n"
                      "c Canceled restarts: 5\n"
                      "c Fast LBD average: 3.250\n"
                      "c Slow LBD average: 4.500\n") != std::string::npos);
}

TEST_CASE("ConsoleStatsWriter: zero denominators omit their ratio lines") {
    RunStats stats;
    stats.file = "x.xnf";
    stats.result = SolveResult::unsat;

    const std::string output = console_output(stats);
    for (const char* ratio : {"Propagations per conflict", "Attempts per propagation",
                              "Fallback decision rate", "Conflicts per second",
                              "Propagations per second"}) {
        CAPTURE(ratio);
        CHECK(output.find(ratio) == std::string::npos);
    }
    // The counts the ratios would divide are still reported.
    CHECK(output.find("c Conflicts: 0\n") != std::string::npos);
    CHECK(output.find("c Propagations: 0\n") != std::string::npos);
    CHECK(output.ends_with("s UNSATISFIABLE\n"));
}

TEST_CASE("ConsoleStatsWriter: the SAT assignment lines") {
    SUBCASE("20 literals per line, one trailing zero") {
        RunStats stats;
        stats.file = "x.xnf";
        stats.result = SolveResult::sat;
        IndexSet assignment(25);
        assignment.set(0);
        assignment.set(4);
        assignment.set(24);
        stats.sat_assignment = assignment;

        const std::string output = console_output(stats);
        CHECK(output.find("s SATISFIABLE\n") != std::string::npos);
        CHECK(output.find("v 1 -2 -3 -4 5 -6 -7 -8 -9 -10 -11 -12 -13 -14 -15 -16 -17 -18 -19 -20\n"
                          "v -21 -22 -23 -24 25 0\n") != std::string::npos);
    }

    SUBCASE("an exact multiple of 20 does not emit an empty line") {
        RunStats stats;
        stats.file = "x.xnf";
        stats.result = SolveResult::sat;
        stats.sat_assignment = IndexSet(20);

        const std::string output = console_output(stats);
        CHECK(output.find("v -1 -2 -3 -4 -5 -6 -7 -8 -9 -10 "
                          "-11 -12 -13 -14 -15 -16 -17 -18 -19 -20 0\n") != std::string::npos);
    }

    SUBCASE("a zero-variable assignment prints exactly `v 0`") {
        RunStats stats;
        stats.file = "x.xnf";
        stats.result = SolveResult::sat;
        stats.sat_assignment = IndexSet(0);
        CHECK(console_output(stats).find("s SATISFIABLE\nv 0\n") != std::string::npos);
    }

    SUBCASE("UNSAT emits no assignment line") {
        RunStats stats;
        stats.file = "x.xnf";
        stats.result = SolveResult::unsat;
        const std::string output = console_output(stats);
        CHECK(output.find("\nv ") == std::string::npos);
        CHECK(output.back() == '\n');
    }
}

TEST_CASE("stats: write_verdict writes the verdict and the model, and nothing else") {
    RunStats stats = sample_stats();
    stats.result = SolveResult::sat;
    stats.sat_assignment = IndexSet(3);

    std::ostringstream out;
    write_verdict(out, stats);
    CHECK(out.str() == "s SATISFIABLE\nv -1 -2 -3 0\n");
}

TEST_CASE("CsvStatsWriter: the header names the configuration and every field") {
    const TempFile temp;
    {
        CsvStatsWriter writer(temp.path());
        writer.log(sample_stats());
    }

    const std::string csv = read_file(temp.path());
    const std::vector<std::string> header = header_of(csv);
    const std::vector<std::string> row = row_of(csv);
    REQUIRE(header.size() == row.size());

    CHECK(header[0] == "run_id");
    CHECK(header[1] == "timestamp_utc");
    CHECK(header[2] == "file");
    CHECK(header[3] == "result");
    CHECK(row[3] == "UNSAT");

    // One prefixed column per option, in the order the row records them.
    CHECK(header[4] == "config.seed");
    CHECK(row[4] == "12345");
    CHECK(header[8] == "config.deletion");
    CHECK(row[8] == "false");

    // Then one column per statistics field, defaults and all: the schema must
    // not depend on the configuration, or two runs would not line up.
    const auto column = [&header](const std::string& name) {
        return std::find(header.begin(), header.end(), name) - header.begin();
    };
    CHECK(row[static_cast<std::size_t>(column("num_vars"))] == "12");
    CHECK(row[static_cast<std::size_t>(column("clauses_in_database"))] == "41");
    CHECK(row[static_cast<std::size_t>(column("clauses_allocated"))] == "47");
    CHECK(row[static_cast<std::size_t>(column("average_learned_lbd"))] == "2.5");
    CHECK(row[static_cast<std::size_t>(column("compactions"))] == "4");
    // Written even though this run reported no deletion section on the console.
    CHECK(row[static_cast<std::size_t>(column("deletion_passes"))] == "4");
    CHECK(row[static_cast<std::size_t>(column("canceled_restarts"))] == "5");
    CHECK(row[static_cast<std::size_t>(column("proof_postprocess_time_s"))] == "0");
}

TEST_CASE("CsvStatsWriter: rows append under one header") {
    const TempFile temp;
    {
        CsvStatsWriter writer(temp.path());
        writer.log(sample_stats());
    }
    const std::string first = read_file(temp.path());

    {
        // A second writer over the same path must not rewrite the header.
        CsvStatsWriter writer(temp.path());
        RunStats stats = sample_stats();
        stats.result = SolveResult::sat;
        writer.log(stats);
    }

    const std::string second = read_file(temp.path());
    CHECK(second.starts_with(first));
    CHECK(second.find("run_id,timestamp_utc", 1) == std::string::npos);
}

TEST_CASE("CsvStatsWriter: the post-processing time is the last column") {
    const TempFile temp;
    CsvStatsWriter writer(temp.path());

    RunStats stats = sample_stats();
    CHECK(stats.proof_postprocess_time_s == 0.0);
    writer.log(stats);
    CHECK(read_file(temp.path()).ends_with(",0\n"));

    stats.proof_postprocess_time_s = 0.4242;
    writer.log(stats);
    CHECK(read_file(temp.path()).ends_with(",0.4242\n"));
}

TEST_CASE("CsvStatsWriter: a value carrying a comma is quoted") {
    const TempFile temp;
    CsvStatsWriter writer(temp.path());

    RunStats stats = sample_stats();
    stats.file = "a,file.xnf";
    writer.log(stats);
    CHECK(read_file(temp.path()).find("\"a,file.xnf\"") != std::string::npos);
}

} // namespace xorcle
