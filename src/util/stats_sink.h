#pragma once

/// @file stats_sink.h
/// @brief Sinks that emit a `RunStats` in the solver's output formats.
///
/// @warning Both formats must stay
/// byte-compatible with the 0.1 binary: the same line keys in the same order,
/// the same conditional omissions, and the same numeric formatting. The 0.2
/// renames (CSIDS, `falsify`) change the *values* the CLI supplies, not the
/// keys.

#include <fstream>
#include <mutex>
#include <optional>
#include <string>

#include "util/stats.h"

namespace xorcle {

/// @brief Thread-safe sink for solver statistics.
class StatsSink {
public:
    /// @brief Virtual destructor for interface use.
    virtual ~StatsSink() = default;

    /// @brief Log stats through the sink with internal synchronization.
    void log(const RunStats& stats);

protected:
    /// @brief Implementation hook for derived sinks.
    virtual void log_impl(const RunStats& stats) = 0;

private:
    std::mutex mu_; ///< Guards log_impl to provide thread safety.
};

/// @brief Log solver statistics to stdout in a human-readable form.
///
/// Emits the DIMACS-style `c` comment lines, the `s SATISFIABLE`/
/// `s UNSATISFIABLE` verdict, and, on SAT, the `v` assignment lines.
class ConsoleStatsSink : public StatsSink {
protected:
    /// @brief Write a human-readable summary to stdout.
    void log_impl(const RunStats& stats) override;
};

/// @brief Log solver statistics to a CSV file.
class CsvStatsSink : public StatsSink {
public:
    /// @brief Construct a CSV sink writing to the given file path.
    /// @throws std::runtime_error If the output file cannot be opened.
    explicit CsvStatsSink(std::string path);

protected:
    /// @brief Write a CSV row for the stats.
    void log_impl(const RunStats& stats) override;

private:
    std::string path_;            ///< CSV output path.
    std::ofstream file_;          ///< Output stream opened in append mode.
    bool header_written_ = false; ///< Whether the CSV header has been written.

    /// @brief Write the CSV header if the file is empty.
    /// @throws std::runtime_error If the header cannot be written.
    void ensure_header();

    /// @brief Escape a CSV field if needed (quotes, commas, newlines).
    static std::string csv_escape(const std::string& value);

    /// @brief Format an optional double for CSV output.
    static std::string fmt_opt_double(const std::optional<double>& value);
};

} // namespace xorcle
