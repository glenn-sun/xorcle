#include "util/StatsLogger.h"

#include <chrono>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace stats {

void StatsSink::log(const AggregateStats& stats) {
    std::lock_guard<std::mutex> lock(mu_);
    log_impl(stats);
}

void ConsoleStatsSink::log_impl(const AggregateStats& stats) {
    std::ostringstream ss;
    ss << "File: " << stats.file << "\n";
    ss << "Decision procedure: " << to_string(stats.decision_procedure) << "\n";
    if (stats.cvsids_learned_base.has_value() && stats.cvsids_decay.has_value() && stats.cvsids_bump.has_value()) {
        ss << "CVsids learned base: " << std::setprecision(6) << *stats.cvsids_learned_base << "\n";
        ss << "CVsids decay: " << std::setprecision(6) << *stats.cvsids_decay << "\n";
        ss << "CVsids bump: " << std::setprecision(6) << *stats.cvsids_bump << "\n";
    }
    ss << "Decision phase: " << to_string(stats.decision_phase) << "\n";
    ss << "Clause selector: " << to_string(stats.clause_selector) << "\n";
    if (stats.runs_requested > 1) {
        ss << "Selected run: median time over " << stats.runs_requested << " runs\n";
    }
    ss << "Variables: " << stats.num_vars << "\n";
    ss << "Start clauses: " << stats.start_clauses << "\n";
    ss << "Final clauses: " << stats.final_clauses << "\n";
    ss << "Learned clauses: " << stats.learned_clauses << "\n";
    ss << "Restarts: " << stats.restarts << "\n";
    ss << "Decisions: " << stats.decisions << "\n";
    if (stats.prop_per_conflict) {
        ss << "Propagations per conflict: " << std::fixed << std::setprecision(3) << *stats.prop_per_conflict << "\n";
    }
    if (stats.attempts_per_prop_watch_conflict) {
        ss << "Attempts per (propagations + watch updates + conflicts): "
           << std::fixed << std::setprecision(3) << *stats.attempts_per_prop_watch_conflict << "\n";
    }
    if (stats.fallback_pct) {
        ss << "Decision fallback: " << std::fixed << std::setprecision(2) << *stats.fallback_pct << "%\n";
    }
    ss << "Time: " << std::fixed << std::setprecision(3) << stats.time_s << " s\n";
    ss << "Runs completed: " << stats.runs_completed << "/" << stats.runs_requested
       << " (timeouts: " << stats.timeout_count << ")\n";
    ss << "Result: " << stats.result << "\n";
    if (stats.runs_requested == 1 && stats.sat_assignment.has_value()) {
        ss << "Satisfying assignment: " << stats.sat_assignment.value() << "\n";
    }
    ss << "\n";

    std::cout << ss.str() << std::flush;
}

CsvStatsSink::CsvStatsSink(std::string path) : path_(std::move(path)) {
    file_.open(path_, std::ios::out | std::ios::app);
    header_written_ = false;
    if (!file_.is_open()) {
        std::cerr << "Error: cannot open CSV file: " << path_ << std::endl;
    }
}

void CsvStatsSink::ensure_header() {
    if (header_written_) {
        return;
    }

    bool need_header = true;
    std::error_code ec;
    if (std::filesystem::exists(path_, ec)) {
        auto size = std::filesystem::file_size(path_, ec);
        if (!ec && size > 0) {
            need_header = false;
        }
    }

    if (need_header) {
        file_ << "run_id,timestamp_utc,file,decision_procedure,cvsids_learned_base,cvsids_decay,cvsids_bump,"
                 "decision_phase,clause_selector,threads,timeout_s,deterministic,"
                 "runs_requested,runs_completed,timeout_count,sat_count,unsat_count,result,"
                 "num_vars,start_clauses,final_clauses,learned_clauses,"
                 "time_s,conflicts,propagations,attempt_props,watch_updates,restarts,"
                 "decisions,fallback_decisions,fallback_pct,"
                 "prop_per_conflict,attempts_per_prop_watch_conflict\n";
    }

    header_written_ = true;
}

std::string CsvStatsSink::csv_escape(const std::string& value) {
    bool needs_quotes = value.find_first_of(",\"\n\r") != std::string::npos;
    if (!needs_quotes) {
        return value;
    }
    std::string out;
    out.reserve(value.size() + 2);
    out.push_back('"');
    for (char c : value) {
        if (c == '"') {
            out.push_back('"');
        }
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}

std::string CsvStatsSink::fmt_opt_double(const std::optional<double>& value) {
    if (!value.has_value()) {
        return "";
    }
    std::ostringstream ss;
    ss << std::setprecision(10) << *value;
    return ss.str();
}

void CsvStatsSink::log_impl(const AggregateStats& stats) {
    if (!file_.is_open()) {
        return;
    }
    ensure_header();

    file_ << csv_escape(stats.run_id) << ','
          << csv_escape(stats.timestamp_utc) << ','
          << csv_escape(stats.file) << ','
          << csv_escape(to_string(stats.decision_procedure)) << ','
          << fmt_opt_double(stats.cvsids_learned_base) << ','
          << fmt_opt_double(stats.cvsids_decay) << ','
          << fmt_opt_double(stats.cvsids_bump) << ','
          << csv_escape(to_string(stats.decision_phase)) << ','
          << csv_escape(to_string(stats.clause_selector)) << ','
          << stats.threads << ','
          << stats.timeout_s << ','
          << (stats.deterministic ? "true" : "false") << ','
          << stats.runs_requested << ','
          << stats.runs_completed << ','
          << stats.timeout_count << ','
          << stats.sat_count << ','
          << stats.unsat_count << ','
          << stats.result << ','
          << stats.num_vars << ','
          << stats.start_clauses << ','
          << stats.final_clauses << ','
          << stats.learned_clauses << ','
          << std::setprecision(10) << stats.time_s << ','
          << stats.conflicts << ','
          << stats.propagations << ','
          << stats.attempt_props << ','
          << stats.watch_updates << ','
          << stats.restarts << ','
          << stats.decisions << ','
          << stats.fallback_decisions << ','
          << fmt_opt_double(stats.fallback_pct) << ','
          << fmt_opt_double(stats.prop_per_conflict) << ','
          << fmt_opt_double(stats.attempts_per_prop_watch_conflict) << "\n";
    file_.flush();
}

} // namespace stats
