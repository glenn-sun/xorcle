#pragma once

#include <fstream>
#include <mutex>
#include <optional>
#include <string>

#include "util/Stats.h"

namespace stats {

/// @brief Thread-safe sink for solver statistics.
class StatsSink {
public:
    /// @brief Virtual destructor for interface use.
    virtual ~StatsSink() = default;

    /// @brief Log stats through the sink with internal synchronization.
    void log(const AggregateStats& stats);

protected:
    /// @brief Implementation hook for derived sinks.
    virtual void log_impl(const AggregateStats& stats) = 0;

private:
    std::mutex mu_; ///< Guards log_impl to provide thread safety.
};

/// @brief Log solver statistics to stdout in a human-readable form.
class ConsoleStatsSink : public StatsSink {
protected:
    /// @brief Write a human-readable summary to stdout.
    void log_impl(const AggregateStats& stats) override;
};

/// @brief Log solver statistics to a CSV file.
class CsvStatsSink : public StatsSink {
public:
    /// @brief Construct a CSV sink writing to the given file path.
    /// @throws std::runtime_error If the output file cannot be opened.
    explicit CsvStatsSink(std::string path);

protected:
    /// @brief Write a CSV row for the stats.
    void log_impl(const AggregateStats& stats) override;

private:
    std::string path_;     ///< CSV output path.
    std::ofstream file_;   ///< Output stream opened in append mode.
    bool header_written_ = false; ///< Whether the CSV header has been written.

    /// @brief Write the CSV header if the file is empty.
    void ensure_header();

    /// @brief Escape a CSV field if needed (quotes, commas, newlines).
    static std::string csv_escape(const std::string& value);

    /// @brief Format an optional double for CSV output.
    static std::string fmt_opt_double(const std::optional<double>& value);
};

} // namespace stats
