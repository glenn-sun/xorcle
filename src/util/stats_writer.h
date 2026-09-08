#pragma once

/// @file stats_writer.h
/// @brief Both formats are generated from `util/stats_fields.inc`, so a field
/// added there reaches the console block and the CSV together.

#include <fstream>
#include <iosfwd>
#include <mutex>
#include <optional>
#include <string>

#include "util/stats.h"

namespace xorcle {

/// @brief Write the `s SATISFIABLE`/`s UNSATISFIABLE` verdict and, on SAT, the
/// `v` assignment lines.
///
/// `ConsoleStatsWriter` ends with this, and a run sending its statistics to a
/// CSV still writes it to stdout: the verdict and the model are the answer, not
/// a statistic, so they are never only in the file.
void write_verdict(std::ostream& out, const RunStats& stats);

/// @brief Thread-safe writer for solver statistics.
class StatsWriter {
public:
    virtual ~StatsWriter() = default;

    void log(const RunStats& stats);

protected:
    virtual void log_impl(const RunStats& stats) = 0;

private:
    std::mutex mu_;
};

/// @brief Log to stdout: the DIMACS-style `c` comment lines, then the verdict
/// and any model that `write_verdict` writes.
class ConsoleStatsWriter : public StatsWriter {
protected:
    void log_impl(const RunStats& stats) override;
};

/// @brief Log solver statistics to a CSV file.
/// @throws std::runtime_error If the output file cannot be opened or written.
class CsvStatsWriter : public StatsWriter {
public:
    explicit CsvStatsWriter(std::string path);

protected:
    void log_impl(const RunStats& stats) override;

private:
    std::string path_;
    std::ofstream file_;          ///< Opened in append mode.
    bool header_written_ = false;

    /// @brief Write the CSV header if the file is empty.
    ///
    /// Takes a row because the configuration columns are named by the options
    /// that row records, which is the whole table in its own order.
    /// @throws std::runtime_error If the header cannot be written.
    void ensure_header(const RunStats& stats);

    static std::string csv_escape(const std::string& value);
};

} // namespace xorcle
