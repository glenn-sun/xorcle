#include "util/stats_writer.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <ostream>
#include <sstream>
#include <stdexcept>

namespace xorcle {

namespace {

constexpr std::size_t assignment_literals_per_line = 20;

/// @brief The prefix a configuration column carries, so a reader can tell the
/// settings of a run apart from what it measured.
constexpr const char* config_column_prefix = "config.";

/// @brief How the console block prints one statistic, by its table kind.
std::string stat_text(StatKind::Count, const std::size_t value) {
    return std::to_string(value);
}
std::string stat_text(StatKind::Real, const double value) {
    std::ostringstream text;
    text << std::fixed << std::setprecision(3) << value;
    return text.str();
}

/// @brief How the CSV writes one, which keeps full precision rather than the
/// three decimals a console reader wants.
std::string csv_text(StatKind::Count, const std::size_t value) {
    return std::to_string(value);
}
std::string csv_text(StatKind::Real, const double value) {
    std::ostringstream text;
    text << std::setprecision(10) << value;
    return text.str();
}

std::string format_ratio(const double value, const int precision) {
    std::ostringstream text;
    text << std::fixed << std::setprecision(precision) << value;
    return text.str();
}

std::optional<double> divide_or_nullopt(const double numer, const double denom) {
    if (denom == 0.0) {
        return std::nullopt;
    }
    return numer / denom;
}

void append_dimacs_assignment(std::ostringstream& output, const IndexSet& assignment) {
    if (assignment.size() == 0) {
        output << "v 0\n";
        return;
    }

    std::size_t line_literal_count = 0;
    output << "v";
    for (std::size_t col_idx = 0; col_idx < assignment.size(); ++col_idx) {
        if (line_literal_count == assignment_literals_per_line) {
            output << "\nv";
            line_literal_count = 0;
        }
        const int literal = assignment.test(col_idx)
                                ? static_cast<int>(col_idx + 1)
                                : -static_cast<int>(col_idx + 1);
        output << " " << literal;
        ++line_literal_count;
    }
    output << " 0\n";
}

/// @brief One `c Label: value` line.
void append_stat(std::ostringstream& output, const char* label, const std::string& value) {
    output << "c " << label << ": " << value << "\n";
}

/// @brief One derived line, omitted entirely when its denominator is zero.
void append_ratio(std::ostringstream& output, const char* label,
                  const std::optional<double>& value, const char* suffix = "",
                  const int precision = 3) {
    if (value.has_value()) {
        output << "c " << label << ": " << format_ratio(*value, precision) << suffix << "\n";
    }
}

} // namespace

void
ConsoleStatsWriter::log(const RunStats& stats) {
    std::ostringstream output;

    // One section's rows, in table order. The sections print in the order the
    // calls below name, which is the order `stats_fields.inc` lists them.
    const auto append_section = [&stats, &output](const StatsSection want) {
        if (!stats.reports(want)) {
            return;
        }
#define XORCLE_STAT(section, kind, name, csv_name, label)                                          \
    if (StatsSection::section == want) {                                                           \
        append_stat(output, label, stat_text(StatKind::kind{}, stats.name));                       \
    }
#include "util/stats_fields.inc"
    };

    append_stat(output, "File", stats.file);
    append_section(StatsSection::instance);

    // What this run was asked to do, which is the part worth reading; the CSV
    // carries all of it, defaults included.
    std::size_t defaults = 0;
    std::ostringstream settings;
    for (const ConfigEntry& entry : stats.config) {
        if (entry.is_default) {
            ++defaults;
            continue;
        }
        settings << "c   " << entry.name << " = " << entry.value << "\n";
    }
    if (defaults == stats.config.size()) {
        output << "c Configuration: all " << defaults << " options at their defaults\n";
    } else {
        output << "c Configuration (" << defaults << " further options at their defaults):\n"
               << settings.str();
    }

    append_section(StatsSection::search);
    append_section(StatsSection::restart);
    append_section(StatsSection::clauses);
    append_section(StatsSection::minimize);
    append_section(StatsSection::subsume);
    append_section(StatsSection::deletion);
    append_section(StatsSection::timing);
    append_section(StatsSection::proof);

    // --- Derived, each omitted when its denominator is zero ---
    append_ratio(output, "Propagations per conflict",
                 divide_or_nullopt(static_cast<double>(stats.num_propagations),
                                   static_cast<double>(stats.num_conflicts)));
    append_ratio(output, "Attempts per propagation, watch update or conflict",
                 divide_or_nullopt(static_cast<double>(stats.num_attempt_props),
                                   static_cast<double>(stats.num_propagations +
                                                       stats.num_watch_updates +
                                                       stats.num_conflicts)));
    append_ratio(output, "Fallback decision rate",
                 divide_or_nullopt(100.0 * static_cast<double>(stats.fallback_decisions),
                                   static_cast<double>(stats.decisions)),
                 "%", 2);
    append_ratio(output, "Conflicts per second",
                 divide_or_nullopt(static_cast<double>(stats.num_conflicts), stats.solve_time_s),
                 "", 1);
    append_ratio(output, "Propagations per second",
                 divide_or_nullopt(static_cast<double>(stats.num_propagations), stats.solve_time_s),
                 "", 1);

    std::cout << output.str();
    write_verdict(std::cout, stats);
    std::cout << std::flush;
}

void
write_verdict(std::ostream& out, const RunStats& stats) {
    std::ostringstream output;
    output << "s " << (stats.result == SolveResult::sat ? "SATISFIABLE" : "UNSATISFIABLE") << "\n";
    if (stats.sat_assignment.has_value()) {
        append_dimacs_assignment(output, *stats.sat_assignment);
    }
    out << output.str() << std::flush;
}

CsvStatsWriter::CsvStatsWriter(std::string path) : path_(std::move(path)) {
    file_.open(path_, std::ios::out | std::ios::app);
    if (!file_.is_open()) {
        throw std::runtime_error("CsvStatsWriter: cannot open CSV file: " + path_);
    }
}

void
CsvStatsWriter::ensure_header(const RunStats& stats) {
    if (header_written_) {
        return;
    }

    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(path_, ec);
    if (ec) {
        throw std::runtime_error("CsvStatsWriter::ensure_header: cannot determine the size of " +
                                 path_ + ": " + ec.message());
    }

    std::ostringstream header;
    header << "run_id,timestamp_utc,file,result";
    for (const ConfigEntry& entry : stats.config) {
        header << ',' << config_column_prefix << entry.name;
    }
#define XORCLE_STAT(section, kind, name, csv_name, label) header << ',' << csv_name;
#include "util/stats_fields.inc"

    if (size == 0) {
        file_ << header.str() << '\n';
        if (!file_) {
            throw std::runtime_error("CsvStatsWriter::ensure_header: failed to write CSV header to " +
                                     path_);
        }
        header_written_ = true;
        return;
    }

    // Appending a row under a header that names other columns silently
    // misaligns every field, so the run stops instead.
    std::ifstream existing(path_);
    std::string first_line;
    if (!existing || !std::getline(existing, first_line)) {
        throw std::runtime_error("CsvStatsWriter::ensure_header: cannot read the first line of " +
                                 path_);
    }
    if (first_line != header.str()) {
        throw std::runtime_error("CsvStatsWriter::ensure_header: " + path_ +
                                 " has a header this build does not write; append to a new file "
                                 "or remove it");
    }

    header_written_ = true;
}

std::string
CsvStatsWriter::csv_escape(const std::string& value) {
    const bool needs_quotes = value.find_first_of(",\"\n\r") != std::string::npos;
    if (!needs_quotes) {
        return value;
    }
    std::string out;
    out.reserve(value.size() + 2);
    out.push_back('"');
    for (const char c : value) {
        if (c == '"') {
            out.push_back('"');
        }
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}

void
CsvStatsWriter::log(const RunStats& stats) {
    ensure_header(stats);

    // Every column is written whatever the configuration, so one file's schema
    // stays the same across a sweep and the header alone describes it.
    file_ << csv_escape(stats.run_id) << ','
          << csv_escape(stats.timestamp_utc) << ','
          << csv_escape(stats.file) << ','
          << to_string(stats.result);
    for (const ConfigEntry& entry : stats.config) {
        file_ << ',' << csv_escape(entry.value);
    }
#define XORCLE_STAT(section, kind, name, csv_name, label)                                          \
    file_ << ',' << csv_text(StatKind::kind{}, stats.name);
#include "util/stats_fields.inc"
    file_ << "\n";

    file_.flush();
    if (!file_) {
        throw std::runtime_error("CsvStatsWriter::log: failed to write a CSV row to " + path_);
    }
}

} // namespace xorcle
