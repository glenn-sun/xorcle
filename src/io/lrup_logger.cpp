#include "io/lrup_logger.h"

#include <cassert>
#include <chrono>
#include <cstddef>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>

namespace xorcle {

namespace {

/// @brief A temporary path next to the final one, unique per logger instance.
std::filesystem::path make_temp_path(const std::filesystem::path& final_path) {
    const auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    std::ostringstream name;
    name << final_path.filename().string() << ".tmp." << now;
    return final_path.parent_path() / name.str();
}

} // namespace

void require_compaction_disabled_for_proof(bool proof_enabled, bool compaction_enabled) {
    if (proof_enabled && compaction_enabled) {
        throw LrupError(std::string{proof_requires_no_compaction_message});
    }
}

LrupLogger::LrupLogger(std::filesystem::path final_path, std::size_t num_vars)
    : final_path_(std::move(final_path)),
      temp_path_(make_temp_path(final_path_)),
      out_(temp_path_),
      num_vars_(num_vars) {
    if (!out_.is_open()) {
        throw LrupError("unable to open proof temp file: " + temp_path_.string());
    }
}

LrupLogger::~LrupLogger() {
    if (finalized_) {
        return;
    }
    out_.close();
    std::error_code ec;
    std::filesystem::remove(temp_path_, ec);
}

void LrupLogger::write_token(const Equation& row) {
    assert(row.size() == num_vars_ + 1 && "proof rows live in the logger's variable space");
    row.bits(bits_);

    // `bits()` is ascending, so the constant bit is last if it is set at all.
    const bool rhs_is_zero = !bits_.empty() && bits_.back() == num_vars_;
    const std::size_t variable_count = bits_.size() - (rhs_is_zero ? 1u : 0u);
    assert(variable_count > 0 && "constant-only rows are not valid proof tokens");

    if (rhs_is_zero) {
        out_ << '-';
    }
    for (std::size_t i = 0; i < variable_count; ++i) {
        if (i != 0) {
            out_ << '+';
        }
        out_ << (bits_[i] + 1);
    }
}

void LrupLogger::write_reasons(const std::vector<ClauseId>& reason_ids) {
    out_ << " 0";
    for (const ClauseId id : reason_ids) {
        out_ << ' ' << (id + 1);
    }
    out_ << " 0\n";
}

void LrupLogger::log_learned_clause(ClauseId new_id, const Clause& clause,
                                    const std::vector<ClauseId>& reason_ids) {
    assert(!finalized_ && "the proof is closed");
    assert(clause.num_vars() == num_vars_ && "the variable space must not change under the logger");

    out_ << (new_id + 1);
    for (const Equation& row : clause) {
        out_ << ' ';
        write_token(row);
    }
    write_reasons(reason_ids);
}

void LrupLogger::log_empty_clause(ClauseId new_id, const std::vector<ClauseId>& reason_ids) {
    assert(!finalized_ && "the proof is closed");

    out_ << (new_id + 1);
    write_reasons(reason_ids);
}

void LrupLogger::finalize(bool keep) {
    if (finalized_) {
        return;
    }
    finalized_ = true;

    out_.flush();
    const bool write_failed = !out_.good();
    out_.close();

    std::error_code ec;
    if (!keep) {
        std::filesystem::remove(temp_path_, ec);
        return;
    }
    if (write_failed) {
        std::filesystem::remove(temp_path_, ec);
        throw LrupError("failed to write proof data to " + temp_path_.string());
    }

    std::filesystem::remove(final_path_, ec);
    ec.clear();
    std::filesystem::rename(temp_path_, final_path_, ec);
    if (!ec) {
        return;
    }

    // Rename fails across filesystems; fall back to a copy so the proof still lands.
    ec.clear();
    std::filesystem::copy_file(temp_path_, final_path_,
                               std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        throw LrupError("failed to write the final proof file: " + final_path_.string());
    }
    ec.clear();
    std::filesystem::remove(temp_path_, ec);
}

} // namespace xorcle
