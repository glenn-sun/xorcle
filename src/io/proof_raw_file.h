#pragma once

/// @file proof_raw_file.h
/// @brief The raw proof on disk: the binary file the solver streams while it
/// runs, and the source post-processing reads it back through.

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

#include "algebra/equation.h"
#include "algebra/row_echelon_matrix.h"
#include "core/clause_id.h"
#include "io/proof_raw.h"

namespace xorcle {

/// @brief A proof file that cannot be written or read back.
class RawProofError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// @brief An output file written to a temporary path next to the final one and
/// moved into place by `finalize(true)`.
///
/// The destructor removes the temporary file unless it was finalized, so an
/// abandoned write leaves nothing behind.
class AtomicOutputFile {
public:
    /// @throws RawProofError If the temporary file cannot be opened.
    AtomicOutputFile(std::filesystem::path final_path, std::ios::openmode mode);

    ~AtomicOutputFile();

    AtomicOutputFile(const AtomicOutputFile&) = delete;
    AtomicOutputFile& operator=(const AtomicOutputFile&) = delete;

    std::ofstream& out() noexcept { return out_; }

    bool finalized() const noexcept { return finalized_; }

    /// @brief Close the file, moving it into place if `keep`.
    /// @throws RawProofError If the file was to be kept and cannot be written.
    /// @note Calling it twice is a no-op the second time.
    void finalize(bool keep);

private:
    std::filesystem::path final_path_;
    std::filesystem::path temp_path_;
    std::ofstream out_;
    bool finalized_ = false;
};

/// @brief One row lifted out of the active numbering into the original one.
///
/// Bit `i` of `row` is variable `active_var_to_original[i]`, and its constant
/// bit `active_var_to_original.size()` is `original_num_vars`.
RawRow lift_row(const Equation& row, std::span<const std::size_t> active_var_to_original,
                std::size_t original_num_vars);

/// @brief Writer for the raw proof.
///
/// The format is a magic string and a version byte, then the variable and input
/// clause counts, then one record per solver event, all as LEB128 varints. Rows
/// are a bit count and then ascending bits, delta encoded.
class RawProofWriter {
public:
    /// @throws RawProofError If the temporary file cannot be opened.
    RawProofWriter(std::filesystem::path final_path, std::size_t num_vars,
                   std::size_t num_input_clauses);

    RawProofWriter(const RawProofWriter&) = delete;
    RawProofWriter& operator=(const RawProofWriter&) = delete;

    /// @brief The original variable count, which every row is written over.
    std::size_t num_vars() const noexcept { return num_vars_; }

    std::size_t num_input_clauses() const noexcept { return num_input_clauses_; }

    /// @brief Write a learned clause, in its linear negation, lifting every row
    /// out of the active numbering.
    /// @pre `reasons` is in replay order and `finalize()` has not run.
    void write_learned(ClauseId id, const Clause& clause,
                       std::span<const std::size_t> active_var_to_original,
                       const std::vector<ClauseId>& reasons);

    /// @brief Write a learned clause whose rows are already over the original
    /// variables.
    /// @pre `reasons` is in replay order and `finalize()` has not run.
    void write_learned(ClauseId id, std::span<const RawRow> rows,
                       const std::vector<ClauseId>& reasons);

    /// @brief Write the final empty clause.
    /// @pre `reasons` is in replay order and `finalize()` has not run.
    void write_empty(ClauseId id, const std::vector<ClauseId>& reasons);

    /// @brief Write the root units a compaction eliminated, in trail order.
    /// @pre `finalize()` has not run.
    void write_compaction(const std::vector<RawUnit>& units);

    /// @brief Write the clauses a deletion pass deleted.
    /// @pre `ids` is non-empty and `finalize()` has not run.
    void write_deleted(const std::vector<ClauseId>& ids);

    /// @brief Close the raw proof, moving it into place if `keep`.
    /// @throws RawProofError If the proof was to be kept and cannot be written.
    void finalize(bool keep) { file_.finalize(keep); }

private:
    AtomicOutputFile file_;
    std::size_t num_vars_;
    std::size_t num_input_clauses_;

    void write_varint(std::size_t value);

    void write_row(const RawRow& row);

    void write_ids(const std::vector<ClauseId>& ids);
};

/// @brief Reads a raw proof file back, record by record.
class RawProofFileSource final : public RawRecordSource {
public:
    /// @throws RawProofError If the file cannot be opened or its header is not a
    /// raw proof of a version this build reads.
    explicit RawProofFileSource(const std::filesystem::path& path);

    std::size_t num_vars() const noexcept { return num_vars_; }

    std::size_t num_input_clauses() const noexcept { return num_input_clauses_; }

    /// @throws RawProofError On a truncated or corrupt record.
    std::optional<RawRecord> next() override;

    void rewind() override;

private:
    std::ifstream in_;
    std::size_t num_vars_ = 0;
    std::size_t num_input_clauses_ = 0;
    std::ifstream::pos_type records_begin_ = 0;

    /// @throws RawProofError At end of file.
    unsigned char read_byte();

    std::size_t read_varint();

    RawRow read_row();

    std::vector<ClauseId> read_ids();
};

} // namespace xorcle
