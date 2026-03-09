#include "util/StatsLogger.h"

#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <sstream>
#include <vector>

namespace stats {

namespace {

constexpr std::size_t kAssignmentLiteralsPerLine = 20;

std::vector<int> parse_assignment_bits(const std::string& assignment_text) {
    std::istringstream input{assignment_text};
    std::vector<int> bits;
    char ch = '\0';
    while (input >> ch) {
        if (ch == '[' || ch == ']') {
            continue;
        }
        input.putback(ch);
        int bit = -1;
        if (!(input >> bit)) {
            break;
        }
        if (bit != 0 && bit != 1) {
            throw std::logic_error("ConsoleStatsSink: satisfying assignment contained a non-binary value");
        }
        bits.push_back(bit);
    }
    return bits;
}

void append_dimacs_assignment(std::ostringstream& output, const std::string& assignment_text) {
    const std::vector<int> bits = parse_assignment_bits(assignment_text);
    if (bits.empty()) {
        output << "v 0\n";
        return;
    }

    std::size_t line_literal_count = 0;
    output << "v";
    for (std::size_t variable_index = 0; variable_index < bits.size(); ++variable_index) {
        if (line_literal_count == kAssignmentLiteralsPerLine) {
            output << "\nv";
            line_literal_count = 0;
        }
        const int literal = bits[variable_index] == 1
                                ? static_cast<int>(variable_index + 1)
                                : -static_cast<int>(variable_index + 1);
        output << " " << literal;
        ++line_literal_count;
    }
    output << " 0\n";
}

} // namespace

void StatsSink::log(const AggregateStats& stats) {
    std::lock_guard<std::mutex> lock(mu_);
    log_impl(stats);
}

void ConsoleStatsSink::log_impl(const AggregateStats& stats) {
    std::ostringstream ss;
    ss << "c File: " << stats.file << "\n";
    ss << "c Variables: " << stats.num_vars << "\n";
    ss << "c Start clauses: " << stats.start_clauses << "\n";
    ss << "c Final clauses: " << stats.final_clauses << "\n";
    ss << "c Learned clauses: " << stats.learned_clauses << "\n";
    ss << "c Decision procedure: " << to_string(stats.decision_procedure) << "\n";
    ss << "c Clause selector: " << to_string(stats.clause_selector) << "\n";
    ss << "c Decision phase: " << to_string(stats.decision_phase) << "\n";
    ss << "c Seed: " << stats.seed << "\n";
    if (stats.cvsids_learned_base.has_value() && stats.cvsids_decay.has_value() && stats.cvsids_bump.has_value()) {
        ss << "c Clause VSIDS learned base: " << std::setprecision(6) << *stats.cvsids_learned_base << "\n";
        ss << "c Clause VSIDS decay: " << std::setprecision(6) << *stats.cvsids_decay << "\n";
        ss << "c Clause VSIDS bump: " << std::setprecision(6) << *stats.cvsids_bump << "\n";
    }
    ss << "c Conflicts: " << stats.conflicts << "\n";
    ss << "c Propagations: " << stats.propagations << "\n";
    ss << "c Watch updates: " << stats.watch_updates << "\n";
    ss << "c Restarts: " << stats.restarts << "\n";
    ss << "c Decisions: " << stats.decisions << "\n";
    if (stats.prop_per_conflict) {
        ss << "c Propagations per conflict: " << std::fixed << std::setprecision(3)
           << *stats.prop_per_conflict << "\n";
    }
    if (stats.attempts_per_prop_watch_conflict) {
        ss << "c Attempts per (propagations + watch updates + conflicts): "
           << std::fixed << std::setprecision(3) << *stats.attempts_per_prop_watch_conflict << "\n";
    }
    if (stats.fallback_pct) {
        ss << "c Decision fallback: " << std::fixed << std::setprecision(2) << *stats.fallback_pct << "%\n";
    }
    ss << "c Time: " << std::fixed << std::setprecision(3) << stats.time_s << " s\n";
    ss << "s " << (stats.result == "SAT" ? "SATISFIABLE" : "UNSATISFIABLE") << "\n";
    if (stats.sat_assignment.has_value()) {
        append_dimacs_assignment(ss, stats.sat_assignment.value());
    }

    std::cout << ss.str() << std::flush;
}

CsvStatsSink::CsvStatsSink(std::string path) : path_(std::move(path)) {
    file_.open(path_, std::ios::out | std::ios::app);
    if (!file_.is_open()) {
        throw std::runtime_error("CsvStatsSink: cannot open CSV file: " + path_);
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
                 "decision_phase,clause_selector,seed,sat_count,unsat_count,result,"
                 "num_vars,start_clauses,final_clauses,learned_clauses,"
                 "time_s,conflicts,propagations,attempt_props,watch_updates,restarts,"
                 "decisions,fallback_decisions,fallback_pct,"
                 "prop_per_conflict,attempts_per_prop_watch_conflict\n";
        if (!file_) {
            throw std::runtime_error("CsvStatsSink::ensure_header: failed to write CSV header to " + path_);
        }
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
          << stats.seed << ','
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
