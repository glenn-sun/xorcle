/// @file test_stats_sink.cpp
/// @brief The console and CSV stats formats.
///
/// The differential harness
/// diffs old against new output, so these are byte-exact expectations rather
/// than shape checks. The strings below were checked against the 0.1 binary's
/// `ConsoleStatsSink` / `CsvStatsSink`.

#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

#include "util/bitmap.h"
#include "util/stats.h"
#include "util/stats_sink.h"

namespace xorcle {

namespace {

/// @brief Capture what a console sink writes to stdout.
std::string console_output(const RunStats& stats) {
    std::ostringstream captured;
    std::streambuf* saved = std::cout.rdbuf(captured.rdbuf());
    ConsoleStatsSink sink;
    sink.log(stats);
    std::cout.rdbuf(saved);
    return captured.str();
}

/// @brief A `RunStats` with every counter set, for format checks.
RunStats sample_stats() {
    RunStats stats;
    stats.file = "tests/generated/random_kxnf_n/random_kxnf_n_n12_k2_s1.xnf";
    stats.num_vars = 12;
    stats.start_clauses = 30;
    stats.final_clauses = 47;
    stats.decision_procedure = "csids";
    stats.clause_selector = "cyclic";
    stats.decision_phase = "falsify";
    stats.seed = 12345;
    stats.result = SolveResult::unsat;
    stats.time_s = 0.0128;
    stats.num_conflicts = 17;
    stats.num_propagations = 200;
    stats.num_attempt_props = 900;
    stats.num_watch_updates = 83;
    stats.num_restarts = 2;
    stats.decisions = 40;
    stats.fallback_decisions = 3;
    return stats;
}

/// @brief A scratch path that removes itself.
class TempFile {
public:
    TempFile()
        : path_(std::filesystem::temp_directory_path() /
                ("xorcle_stats_test_" + std::to_string(std::rand()) + ".csv")) {
        std::filesystem::remove(path_);
    }
    ~TempFile() { std::filesystem::remove(path_); }

    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;

    const std::string& path() const { return path_string_; }

private:
    std::filesystem::path path_;
    std::string path_string_ = path_.string();
};

/// @brief Read a whole file.
std::string read_file(const std::string& path) {
    std::ifstream in(path);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

} // namespace

TEST_CASE("learned_clause_count floors at zero") {
    RunStats stats;
    stats.start_clauses = 30;
    stats.final_clauses = 47;
    CHECK(learned_clause_count(stats) == 17);

    // Compaction can leave fewer clauses than the run started with.
    stats.final_clauses = 12;
    CHECK(learned_clause_count(stats) == 0);
}

TEST_CASE("to_string(SolveResult) uses the CSV spellings") {
    CHECK(to_string(SolveResult::sat) == "SAT");
    CHECK(to_string(SolveResult::unsat) == "UNSAT");
}

TEST_CASE("ConsoleStatsSink: the UNSAT format") {
    const std::string expected =
        "c File: tests/generated/random_kxnf_n/random_kxnf_n_n12_k2_s1.xnf\n"
        "c Variables: 12\n"
        "c Start clauses: 30\n"
        "c Final clauses: 47\n"
        "c Learned clauses: 17\n"
        "c Decision procedure: csids\n"
        "c Clause selector: cyclic\n"
        "c Decision phase: falsify\n"
        "c Seed: 12345\n"
        "c Conflicts: 17\n"
        "c Propagations: 200\n"
        "c Attempted propagations: 900\n"
        "c Watch updates: 83\n"
        "c Restarts: 2\n"
        "c Decisions: 40\n"
        "c Fallback decisions: 3\n"
        "c Propagations per conflict: 11.765\n"
        "c Attempts per (propagations + watch updates + conflicts): 3.000\n"
        "c Fallback decision rate: 7.50%\n"
        "c Time: 0.013 s\n"
        "s UNSATISFIABLE\n";
    CHECK(console_output(sample_stats()) == expected);
}

TEST_CASE("ConsoleStatsSink: the CSIDS lines appear together or not at all") {
    RunStats stats = sample_stats();

    SUBCASE("all three set") {
        stats.csids_initial_activity = 1.0;
        stats.csids_decay = 0.98;
        stats.csids_bump = 1.0;
        const std::string output = console_output(stats);
        // The 0.1 line keys are kept verbatim -- the format is byte-compatible
        // even though the identifiers behind it were renamed to CSIDS.
        CHECK(output.find("c Clause VSIDS learned base: 1\n") != std::string::npos);
        CHECK(output.find("c Clause VSIDS decay: 0.98\n") != std::string::npos);
        CHECK(output.find("c Clause VSIDS bump: 1\n") != std::string::npos);
        // They sit between the seed and the conflict counter.
        CHECK(output.find("c Seed: 12345\nc Clause VSIDS learned base:") != std::string::npos);
        CHECK(output.find("c Clause VSIDS bump: 1\nc Conflicts:") != std::string::npos);
    }

    SUBCASE("only two set") {
        stats.csids_initial_activity = 1.0;
        stats.csids_decay = 0.98;
        CHECK(console_output(stats).find("Clause VSIDS") == std::string::npos);
    }

    SUBCASE("none set") {
        CHECK(console_output(stats).find("Clause VSIDS") == std::string::npos);
    }
}

TEST_CASE("ConsoleStatsSink: the CSIDS lines print in default float format") {
    RunStats stats = sample_stats();
    stats.csids_initial_activity = 0.000001234567;
    stats.csids_decay = 0.98;
    stats.csids_bump = 1.5;
    const std::string output = console_output(stats);
    // `setprecision(6)` without `std::fixed`: six significant digits, and small
    // magnitudes go to scientific notation rather than rounding to zero.
    CHECK(output.find("c Clause VSIDS learned base: 1.23457e-06\n") != std::string::npos);
    // The sticky flags must not leak into the fixed-format lines that follow.
    CHECK(output.find("c Time: 0.013 s\n") != std::string::npos);
}

TEST_CASE("ConsoleStatsSink: zero denominators omit their ratio lines") {
    RunStats stats;
    stats.file = "x.xnf";
    stats.result = SolveResult::unsat;

    const std::string expected =
        "c File: x.xnf\n"
        "c Variables: 0\n"
        "c Start clauses: 0\n"
        "c Final clauses: 0\n"
        "c Learned clauses: 0\n"
        "c Decision procedure: \n"
        "c Clause selector: \n"
        "c Decision phase: \n"
        "c Seed: 0\n"
        "c Conflicts: 0\n"
        "c Propagations: 0\n"
        "c Attempted propagations: 0\n"
        "c Watch updates: 0\n"
        "c Restarts: 0\n"
        "c Decisions: 0\n"
        "c Fallback decisions: 0\n"
        "c Time: 0.000 s\n"
        "s UNSATISFIABLE\n";
    CHECK(console_output(stats) == expected);
}

TEST_CASE("ConsoleStatsSink: the SAT assignment lines") {
    SUBCASE("20 literals per line, one trailing zero") {
        RunStats stats;
        stats.file = "x.xnf";
        stats.result = SolveResult::sat;
        Bitmap assignment(25);
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
        stats.sat_assignment = Bitmap(20);

        const std::string output = console_output(stats);
        CHECK(output.find("v -1 -2 -3 -4 -5 -6 -7 -8 -9 -10 "
                          "-11 -12 -13 -14 -15 -16 -17 -18 -19 -20 0\n") != std::string::npos);
    }

    SUBCASE("a zero-width assignment prints exactly `v 0`") {
        RunStats stats;
        stats.file = "x.xnf";
        stats.result = SolveResult::sat;
        stats.sat_assignment = Bitmap(0);
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

TEST_CASE("CsvStatsSink: header then rows, appended") {
    const TempFile temp;

    {
        CsvStatsSink sink(temp.path());
        sink.log(sample_stats());
    }

    const std::string first = read_file(temp.path());
    CHECK(first.starts_with(
        "run_id,timestamp_utc,file,decision_procedure,cvsids_learned_base,cvsids_decay,cvsids_bump,"
        "decision_phase,clause_selector,seed,result,"
        "num_vars,start_clauses,final_clauses,learned_clauses,"
        "time_s,conflicts,propagations,attempt_props,watch_updates,restarts,"
        "decisions,fallback_decisions,fallback_pct,"
        "prop_per_conflict,attempts_per_prop_watch_conflict\n"));
    // Absent optional doubles are empty fields, and the result column is the
    // short spelling, not the console's SATISFIABLE/UNSATISFIABLE.
    CHECK(first.find(",csids,,,,falsify,cyclic,12345,UNSAT,12,30,47,17,") != std::string::npos);

    {
        // A second sink over the same path must not rewrite the header.
        CsvStatsSink sink(temp.path());
        RunStats stats = sample_stats();
        stats.result = SolveResult::sat;
        sink.log(stats);
    }

    const std::string second = read_file(temp.path());
    CHECK(second.starts_with(first));
    CHECK(second.find("run_id,timestamp_utc", 1) == std::string::npos);
}

TEST_CASE("CsvStatsSink: fields containing separators are quoted") {
    const TempFile temp;
    CsvStatsSink sink(temp.path());

    RunStats stats = sample_stats();
    stats.file = R"(a,b "quoted" path)";
    stats.csids_initial_activity = 1.0;
    stats.csids_decay = 0.98;
    stats.csids_bump = 1.0;
    sink.log(stats);

    const std::string written = read_file(temp.path());
    CHECK(written.find(R"("a,b ""quoted"" path")") != std::string::npos);
    CHECK(written.find(",1,0.98,1,") != std::string::npos);
}

TEST_CASE("CsvStatsSink: an unopenable path is a user-facing error") {
    // Error policy: bad output paths come from the command
    // line, so they throw rather than assert.
    CHECK_THROWS_AS(CsvStatsSink("/nonexistent-directory-for-xorcle-tests/out.csv"),
                    std::runtime_error);
}

} // namespace xorcle
