#include "util/stats_writer.h"

#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace xorcle {

namespace {

constexpr std::size_t assignment_literals_per_line = 20;

std::optional<double> divide_or_nullopt(const double numer, const double denom) {
    if (denom == 0.0) {
        return std::nullopt;
    }
    return numer / denom;
}

std::optional<double> fallback_pct(const RunStats& stats) {
    return divide_or_nullopt(100.0 * static_cast<double>(stats.fallback_decisions),
                             static_cast<double>(stats.decisions));
}

std::optional<double> propagations_per_conflict(const RunStats& stats) {
    return divide_or_nullopt(static_cast<double>(stats.num_propagations),
                             static_cast<double>(stats.num_conflicts));
}

std::optional<double> attempts_per_event(const RunStats& stats) {
    return divide_or_nullopt(static_cast<double>(stats.num_attempt_props),
                             static_cast<double>(stats.num_propagations + stats.num_watch_updates +
                                                 stats.num_conflicts));
}

void append_dimacs_assignment(std::ostringstream& output, const IndexSet& assignment) {
    if (assignment.size() == 0) {
        output << "v 0\n";
        return;
    }

    std::size_t line_literal_count = 0;
    output << "v";
    for (std::size_t variable_index = 0; variable_index < assignment.size(); ++variable_index) {
        if (line_literal_count == assignment_literals_per_line) {
            output << "\nv";
            line_literal_count = 0;
        }
        const int literal = assignment.test(variable_index)
                                ? static_cast<int>(variable_index + 1)
                                : -static_cast<int>(variable_index + 1);
        output << " " << literal;
        ++line_literal_count;
    }
    output << " 0\n";
}

} // namespace

void
StatsWriter::log(const RunStats& stats) {
    std::lock_guard<std::mutex> lock(mu_);
    log_impl(stats);
}

void
ConsoleStatsWriter::log_impl(const RunStats& stats) {
    std::ostringstream output;
    output << "c File: " << stats.file << "\n";
    output << "c Variables: " << stats.num_vars << "\n";
    output << "c Start clauses: " << stats.start_clauses << "\n";
    output << "c Final clauses: " << stats.final_clauses << "\n";
    output << "c Learned clauses: " << learned_clause_count(stats) << "\n";
    output << "c Decision procedure: " << stats.decision_procedure << "\n";
    output << "c Clause selector: " << stats.clause_selector << "\n";
    output << "c Decision phase: " << stats.decision_phase << "\n";
    output << "c Seed: " << stats.seed << "\n";
    // The three CSIDS lines appear together or not at all, and use bare
    // `setprecision(6)` -- no `std::fixed` -- so they print in default float
    // format. Every other formatted line sets its own flags immediately before
    // its value, so sticky stream state never leaks between lines.
    if (stats.csids_initial_activity.has_value() && stats.csids_decay.has_value() &&
        stats.csids_bump.has_value()) {
        output << "c CSIDS initial activity: " << std::setprecision(6) << *stats.csids_initial_activity << "\n";
        output << "c CSIDS decay: " << std::setprecision(6) << *stats.csids_decay << "\n";
        output << "c CSIDS bump: " << std::setprecision(6) << *stats.csids_bump << "\n";
    }
    output << "c Conflicts: " << stats.num_conflicts << "\n";
    output << "c Propagations: " << stats.num_propagations << "\n";
    output << "c Attempted propagations: " << stats.num_attempt_props << "\n";
    output << "c Watch updates: " << stats.num_watch_updates << "\n";
    output << "c Restarts: " << stats.num_restarts << "\n";
    output << "c Decisions: " << stats.decisions << "\n";
    output << "c Fallback decisions: " << stats.fallback_decisions << "\n";
    // Each ratio line is omitted entirely when its denominator is zero; a
    // parser must not assume it is present.
    if (const auto ratio = propagations_per_conflict(stats); ratio.has_value()) {
        output << "c Propagations per conflict: " << std::fixed << std::setprecision(3) << *ratio << "\n";
    }
    if (const auto ratio = attempts_per_event(stats); ratio.has_value()) {
        output << "c Attempts per (propagations + watch updates + conflicts): "
               << std::fixed << std::setprecision(3) << *ratio << "\n";
    }
    if (const auto ratio = fallback_pct(stats); ratio.has_value()) {
        output << "c Fallback decision rate: " << std::fixed << std::setprecision(2) << *ratio << "%\n";
    }
    output << "c Time: " << std::fixed << std::setprecision(3) << stats.time_s << " s\n";
    output << "s " << (stats.result == SolveResult::sat ? "SATISFIABLE" : "UNSATISFIABLE") << "\n";
    if (stats.sat_assignment.has_value()) {
        append_dimacs_assignment(output, *stats.sat_assignment);
    }

    std::cout << output.str() << std::flush;
}

CsvStatsWriter::CsvStatsWriter(std::string path) : path_(std::move(path)) {
    file_.open(path_, std::ios::out | std::ios::app);
    if (!file_.is_open()) {
        throw std::runtime_error("CsvStatsWriter: cannot open CSV file: " + path_);
    }
}

void
CsvStatsWriter::ensure_header() {
    if (header_written_) {
        return;
    }

    bool need_header = true;
    std::error_code ec;
    if (std::filesystem::exists(path_, ec)) {
        const auto size = std::filesystem::file_size(path_, ec);
        if (!ec && size > 0) {
            need_header = false;
        }
    }

    if (need_header) {
        file_ << "run_id,timestamp_utc,file,decision_procedure,csids_initial_activity,csids_decay,csids_bump,"
                 "decision_phase,clause_selector,seed,result,"
                 "num_vars,start_clauses,final_clauses,learned_clauses,"
                 "time_s,conflicts,propagations,attempt_props,watch_updates,restarts,"
                 "decisions,fallback_decisions,fallback_pct,"
                 "prop_per_conflict,attempts_per_prop_watch_conflict\n";
        if (!file_) {
            throw std::runtime_error("CsvStatsWriter::ensure_header: failed to write CSV header to " + path_);
        }
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

std::string
CsvStatsWriter::fmt_opt_double(const std::optional<double>& value) {
    if (!value.has_value()) {
        return "";
    }
    std::ostringstream output;
    output << std::setprecision(10) << *value;
    return output.str();
}

void
CsvStatsWriter::log_impl(const RunStats& stats) {
    ensure_header();

    file_ << csv_escape(stats.run_id) << ','
          << csv_escape(stats.timestamp_utc) << ','
          << csv_escape(stats.file) << ','
          << csv_escape(stats.decision_procedure) << ','
          << fmt_opt_double(stats.csids_initial_activity) << ','
          << fmt_opt_double(stats.csids_decay) << ','
          << fmt_opt_double(stats.csids_bump) << ','
          << csv_escape(stats.decision_phase) << ','
          << csv_escape(stats.clause_selector) << ','
          << stats.seed << ','
          << to_string(stats.result) << ','
          << stats.num_vars << ','
          << stats.start_clauses << ','
          << stats.final_clauses << ','
          << learned_clause_count(stats) << ','
          << std::setprecision(10) << stats.time_s << ','
          << stats.num_conflicts << ','
          << stats.num_propagations << ','
          << stats.num_attempt_props << ','
          << stats.num_watch_updates << ','
          << stats.num_restarts << ','
          << stats.decisions << ','
          << stats.fallback_decisions << ','
          << fmt_opt_double(fallback_pct(stats)) << ','
          << fmt_opt_double(propagations_per_conflict(stats)) << ','
          << fmt_opt_double(attempts_per_event(stats)) << "\n";
    file_.flush();
}

} // namespace xorcle
