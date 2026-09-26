#pragma once

/// @file stats_writer.h
/// @brief The console and CSV formats of a run's statistics.

#include <fstream>
#include <iosfwd>
#include <string>

#include "util/stats.h"

namespace xorcle {

/// @brief Write the `s SATISFIABLE`/`s UNSATISFIABLE` verdict and, on SAT, the
/// `v` assignment lines.
///
/// `ConsoleStatsWriter` ends with this, and a run sending its statistics to a
/// CSV still writes it to stdout.
void write_verdict(std::ostream& out, const RunStats& stats);

/// @brief Writer for solver statistics.
class StatsWriter {
public:
    virtual ~StatsWriter() = default;

    virtual void log(const RunStats& stats) = 0;
};

/// @brief Log to stdout: the DIMACS-style `c` comment lines, then the verdict
/// and any model that `write_verdict` writes.
class ConsoleStatsWriter final : public StatsWriter {
public:
    void log(const RunStats& stats) override;
};

/// @brief Log solver statistics to a CSV file.
/// @throws std::runtime_error If the output file cannot be opened or written.
class CsvStatsWriter final : public StatsWriter {
public:
    explicit CsvStatsWriter(std::string path);

    void log(const RunStats& stats) override;

private:
    std::string path_;
    std::ofstream file_;          ///< Opened in append mode.
    bool header_written_ = false;

    /// @brief Write the CSV header to an empty file, or check that an existing
    /// file's header matches.
    ///
    /// The configuration columns are named by the options `stats` records.
    /// @throws std::runtime_error If the header cannot be written or does not match.
    void ensure_header(const RunStats& stats);

    static std::string csv_escape(const std::string& value);
};

} // namespace xorcle
