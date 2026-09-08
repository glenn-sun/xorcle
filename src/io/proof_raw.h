#pragma once

/// @file proof_raw.h
/// @brief The raw proof: the records the solver streams to a temporary file
/// while it runs, which post-processing turns into an LRUP(⊕) proof.
///
/// Every row is over the *original* variables, whatever compaction has done to
/// the solver's numbering. Ids are solver `ClauseId`s, 0-based, with the input
/// clauses at `0 .. num_input_clauses - 1`; post-processing renumbers.

#include <cstddef>
#include <utility>
#include <variant>
#include <vector>

#include "core/clause_id.h"

namespace xorcle {

/// @brief One equation: ascending bit indices, with `num_vars` standing for the
/// constant column, exactly as `Equation::bits()` reports it.
using RawRow = std::vector<std::size_t>;

/// @brief A learned clause in its linear negation, reasons in replay order.
struct RawLearned {
    ClauseId id;
    std::vector<RawRow> rows;
    std::vector<ClauseId> reasons;
};

/// @brief The final empty clause, reasons in replay order.
struct RawEmpty {
    ClauseId id;
    std::vector<ClauseId> reasons;
};

/// @brief One root trail row a compaction eliminated, and the clause that
/// propagated it. Its derivation depends only on units before it in the record.
/// The row is a trail row, so it is reduced only on its leading column.
struct RawUnit {
    RawRow row;
    ClauseId reason;
};

/// @brief A compaction: the whole root trail it eliminated, in trail order.
struct RawCompaction {
    std::vector<RawUnit> units;
};

/// @brief Clauses the solver deleted. No later record names them; a memory hint.
struct RawDeleted {
    std::vector<ClauseId> ids;
};

using RawRecord = std::variant<RawLearned, RawEmpty, RawCompaction, RawDeleted>;

/// @brief A rewindable stream of raw records.
class RawRecordSource {
public:
    virtual ~RawRecordSource() = default;

    /// @return False at the end of the stream, leaving `record` untouched.
    virtual bool next(RawRecord& record) = 0;

    virtual void rewind() = 0;
};

/// @brief An in-memory source, for tests.
class RawRecordVectorSource final : public RawRecordSource {
public:
    explicit RawRecordVectorSource(std::vector<RawRecord> records)
        : records_(std::move(records)) {}

    bool next(RawRecord& record) override {
        if (position_ == records_.size()) {
            return false;
        }
        record = records_[position_++];
        return true;
    }

    void rewind() override { position_ = 0; }

private:
    std::vector<RawRecord> records_;
    std::size_t position_ = 0;
};

} // namespace xorcle
