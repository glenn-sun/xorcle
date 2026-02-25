#include "io/ProofLogger.h"

#include <chrono>
#include <sstream>
#include <stdexcept>

namespace io {

namespace {
constexpr char kCommentPrefix = 'c';
}

ProofLogger::ProofLogger(const std::string& final_path)
    : final_path_(final_path),
      temp_path_(make_temp_path(final_path_)),
      out_(temp_path_) {
    if (!out_.is_open()) {
        throw std::runtime_error("Unable to open proof temp file: " + temp_path_.string());
    }
}

ProofLogger::~ProofLogger() {
    if (!finalized_) {
        try {
            out_.close();
        } catch (...) {
        }
        std::error_code ec;
        std::filesystem::remove(temp_path_, ec);
    }
}

void ProofLogger::log_comment(const std::string& comment) {
    if (!out_.is_open()) return;
    out_ << kCommentPrefix << " " << comment << "\n";
}

bool ProofLogger::equation_to_token(const bit::vector<>& row, std::string& out_token) {
    const std::size_t constant_idx = row.size() - 1;
    const bool rhs_is_one = !row.back();

    std::ostringstream oss;
    bool any_var = false;
    row.if_set_call([&](std::size_t i) {
        if (i == constant_idx) return;
        if (any_var) oss << '+';
        oss << (i + 1);
        any_var = true;
    });

    if (!any_var) {
        return false;
    }

    if (!rhs_is_one) {
        out_token = "-" + oss.str();
    } else {
        out_token = oss.str();
    }
    return true;
}

void ProofLogger::log_learned_clause(heuristics::ClauseId new_id,
                                     const algebra::Clause& clause,
                                     const std::vector<heuristics::ClauseId>& reason_ids) {
    if (!out_.is_open()) return;

    out_ << (new_id + 1);
    for (const auto& row : clause) {
        std::string token;
        if (!equation_to_token(row, token)) {
            log_comment("Skipping constant-only equation in clause id " + std::to_string(new_id + 1));
            return;
        }
        out_ << " " << token;
    }
    out_ << " 0";
    for (heuristics::ClauseId id : reason_ids) {
        out_ << " " << (id + 1);
    }
    out_ << " 0\n";
}

void ProofLogger::log_empty_clause(heuristics::ClauseId new_id,
                                   const std::vector<heuristics::ClauseId>& reason_ids) {
    if (!out_.is_open()) return;

    out_ << (new_id + 1) << " 0";
    for (heuristics::ClauseId id : reason_ids) {
        out_ << " " << (id + 1);
    }
    out_ << " 0\n";
}

void ProofLogger::log_delete_clauses(const std::vector<heuristics::ClauseId>& clause_ids) {
    if (!out_.is_open() || clause_ids.empty()) return;
    bool first = true;
    for (heuristics::ClauseId id : clause_ids) {
        if (!first) out_ << " ";
        out_ << (id + 1);
        first = false;
    }
    out_ << " 0\n";
}

void ProofLogger::finalize(bool keep) {
    if (finalized_) return;
    finalized_ = true;
    out_.flush();
    out_.close();

    std::error_code ec;
    if (keep) {
        std::filesystem::remove(final_path_, ec);
        ec.clear();
        std::filesystem::rename(temp_path_, final_path_, ec);
        if (ec) {
            std::filesystem::copy_file(temp_path_, final_path_,
                                       std::filesystem::copy_options::overwrite_existing, ec);
            std::filesystem::remove(temp_path_, ec);
        }
    } else {
        std::filesystem::remove(temp_path_, ec);
    }
}

std::filesystem::path ProofLogger::make_temp_path(const std::filesystem::path& final_path) const {
    auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    std::ostringstream ss;
    ss << final_path.filename().string() << ".tmp." << now;
    return final_path.parent_path() / ss.str();
}

} // namespace io
