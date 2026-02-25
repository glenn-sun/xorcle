#pragma once

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "algebra/RowSpace.h"
#include "bit/vector.h"
#include "heuristics/Types.h"

namespace io {

/// @brief Logger for DRUP(⊕) proof output.
class ProofLogger {
public:
    /// @brief Create a proof logger writing to a temporary file.
    explicit ProofLogger(const std::string& final_path);

    /// @brief Destructor cleans up temp file if not finalized.
    ~ProofLogger();

    ProofLogger(const ProofLogger&) = delete;
    ProofLogger& operator=(const ProofLogger&) = delete;

    /// @brief Log a comment line.
    void log_comment(const std::string& comment);

    /// @brief Log a learned clause line.
    /// @param new_id Clause id (0-based).
    /// @param clause Learned clause.
    /// @param reason_ids Clause ids (0-based) in unit-propagation order.
    void log_learned_clause(heuristics::ClauseId new_id,
                            const algebra::Clause& clause,
                            const std::vector<heuristics::ClauseId>& reason_ids);

    /// @brief Log a final empty clause line.
    /// @param new_id Clause id (0-based).
    /// @param reason_ids Clause ids (0-based) in unit-propagation order.
    void log_empty_clause(heuristics::ClauseId new_id,
                          const std::vector<heuristics::ClauseId>& reason_ids);

    /// @brief Log clause deletions (not used by the solver yet).
    void log_delete_clauses(const std::vector<heuristics::ClauseId>& clause_ids);

    /// @brief Finalize the proof file.
    /// @param keep If true, move temp file to final path; otherwise delete it.
    void finalize(bool keep);

private:
    std::filesystem::path final_path_;
    std::filesystem::path temp_path_;
    std::ofstream out_;
    bool finalized_{false};

    std::filesystem::path make_temp_path(const std::filesystem::path& final_path) const;
    static bool equation_to_token(const bit::vector<>& row, std::string& out_token);
};

} // namespace io
