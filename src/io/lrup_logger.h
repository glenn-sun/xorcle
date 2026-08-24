#pragma once

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "algebra/equation.h"
#include "algebra/row_echelon_matrix.h"
#include "core/clause_id.h"

namespace xorcle {

/// @brief A proof that cannot be written, or a configuration that cannot produce one.
class LrupError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

inline constexpr std::string_view proof_requires_no_compaction_message =
    "--proof requires --compact=false: proof emission with compaction enabled is not "
    "supported";

/// @throws LrupError With `proof_requires_no_compaction_message` if both are true.
void require_compaction_disabled_for_proof(bool proof_enabled, bool compaction_enabled);

/// @brief Writer for LRUP(⊕) proofs, in the line grammar `README.md` documents.
///
/// Input clauses are never written; they take implicit ids counting from 1, in
/// the order `parse_dimacs()` returns them. `ClauseId`s are 0-based, so the
/// logger adds one to every id it writes.
///
/// Reason ids must arrive in derivation order; conflict analysis produces the
/// opposite order, so the solver reverses the collected list exactly once.
///
/// Lines go to a temporary file next to the final path, moved into place by
/// `finalize(true)`.
class LrupLogger {
public:
    /// @throws LrupError If the temporary file cannot be opened.
    LrupLogger(std::filesystem::path final_path, std::size_t num_vars);

    /// @brief Delete the temporary file unless the proof was finalized.
    ~LrupLogger();

    LrupLogger(const LrupLogger&) = delete;
    LrupLogger& operator=(const LrupLogger&) = delete;

    std::size_t num_vars() const noexcept { return num_vars_; }

    /// @brief Write one learned-clause line.
    /// @param clause The learned clause, in its linear negation.
    /// @pre `clause.num_vars() == num_vars()`, no row of `clause` is
    /// constant-only, and `finalize()` has not run.
    void log_learned_clause(ClauseId new_id, const Clause& clause,
                            const std::vector<ClauseId>& reason_ids);

    /// @brief Write the final empty-clause line.
    /// @pre `finalize()` has not run.
    void log_empty_clause(ClauseId new_id, const std::vector<ClauseId>& reason_ids);

    /// @brief Close the proof, moving the temporary file into place if `keep`.
    /// @throws LrupError If the proof was to be kept and cannot be written.
    /// @note Calling it twice is a no-op the second time.
    void finalize(bool keep);

private:
    std::filesystem::path final_path_;
    std::filesystem::path temp_path_;
    std::ofstream out_;
    std::size_t num_vars_;
    bool finalized_ = false;

    void write_reasons(const std::vector<ClauseId>& reason_ids);

    void write_token(const Equation& row);
};

} // namespace xorcle
