#pragma once

/// @file stats_writer.h

#include <fstream>
#include <mutex>
#include <optional>
#include <string>

#include "util/stats.h"

namespace xorcle {

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

/// @brief Log to stdout: the DIMACS-style `c` comment lines, the
/// `s SATISFIABLE`/`s UNSATISFIABLE` verdict, and on SAT the `v` lines.
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
    /// @throws std::runtime_error If the header cannot be written.
    void ensure_header();

    static std::string csv_escape(const std::string& value);

    static std::string fmt_opt_double(const std::optional<double>& value);
};

} // namespace xorcle
