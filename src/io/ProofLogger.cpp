#include "io/ProofLogger.h"

#include <chrono>
#include <sstream>
#include <stdexcept>

namespace io {

namespace {
std::string make_stream_error_message(const char* function_name) {
    return std::string{"ProofLogger::"} + function_name;
}
} // namespace

ProofLogger::ProofLogger(const std::string& final_path)
    : final_path_(final_path),
      temp_path_(make_temp_path(final_path_)),
      out_(temp_path_) {
    if (!out_.is_open()) {
        throw std::runtime_error("Unable to open proof temp file: " + temp_path_.string());
    }
}

void ProofLogger::ensure_writable(const char* function_name) const {
    if (finalized_) {
        throw std::logic_error(make_stream_error_message(function_name) +
                               ": cannot write after finalize()");
    }
    if (!out_.is_open()) {
        throw std::logic_error(make_stream_error_message(function_name) +
                               ": proof temp stream is not open");
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

std::string ProofLogger::equation_to_token(const bit::vector<>& row) {
    if (row.size() == 0) {
        throw std::logic_error("ProofLogger::equation_to_token: proof rows must be non-empty");
    }
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
        throw std::logic_error("ProofLogger::equation_to_token: constant-only rows cannot be emitted as learned-clause tokens");
    }

    if (!rhs_is_one) {
        return "-" + oss.str();
    }
    return oss.str();
}

void ProofLogger::log_learned_clause(heuristics::ClauseId new_id,
                                     const algebra::Clause& clause,
                                     const std::vector<heuristics::ClauseId>& reason_ids) {
    ensure_writable("log_learned_clause");

    out_ << (new_id + 1);
    for (const auto& row : clause) {
        out_ << " " << equation_to_token(row);
    }
    out_ << " 0";
    for (heuristics::ClauseId id : reason_ids) {
        out_ << " " << (id + 1);
    }
    out_ << " 0\n";
}

void ProofLogger::log_empty_clause(heuristics::ClauseId new_id,
                                   const std::vector<heuristics::ClauseId>& reason_ids) {
    ensure_writable("log_empty_clause");

    out_ << (new_id + 1) << " 0";
    for (heuristics::ClauseId id : reason_ids) {
        out_ << " " << (id + 1);
    }
    out_ << " 0\n";
}

void ProofLogger::finalize(bool keep) {
    if (finalized_) {
        return;
    }
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
            if (ec) {
                throw std::runtime_error("ProofLogger::finalize: failed to write final proof file: " +
                                         final_path_.string());
            }
            ec.clear();
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
