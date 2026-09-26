#pragma once

/// @file proof_raw.h
/// @brief The raw proof: the records the solver streams to a temporary file
/// while it runs, which post-processing turns into an LRUP(⊕) proof.
///
/// Every row is over the *original* variables, whatever compaction has done to
/// the solver's numbering. Ids are 0-based, with the input clauses at
/// `0 .. num_input_clauses - 1` and every later line taking the next one, root
/// units included; post-processing renumbers.

#include <cstddef>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

#include "core/clause_id.h"

namespace xorcle {

/// @brief One equation: ascending bit indices, with `num_vars` standing for the
/// constant bit, exactly as `Equation::bits()` reports it.
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

/// @brief One root unit a compaction eliminated, and the line proving it.
///
/// The row is reduced by every unit before it in the record, so the rows stand
/// as an echelon basis in record order, and the line proves the unit clause of
/// exactly this equation.
struct RawUnit {
    RawRow row;
    ClauseId id;
};

/// @brief A compaction: every root unit it eliminated, in trail order.
struct RawCompaction {
    std::vector<RawUnit> units;
};

/// @brief Clauses the solver deleted, which no later record names.
struct RawDeleted {
    std::vector<ClauseId> ids;
};

using RawRecord = std::variant<RawLearned, RawEmpty, RawCompaction, RawDeleted>;

/// @brief A rewindable stream of raw records.
class RawRecordSource {
public:
    virtual ~RawRecordSource() = default;

    /// @return The next record, or `std::nullopt` at the end of the stream.
    virtual std::optional<RawRecord> next() = 0;

    virtual void rewind() = 0;
};

/// @brief An in-memory source, for tests.
class RawRecordVectorSource final : public RawRecordSource {
public:
    explicit RawRecordVectorSource(std::vector<RawRecord> records)
        : records_(std::move(records)) {}

    std::optional<RawRecord> next() override {
        if (position_ == records_.size()) {
            return std::nullopt;
        }
        return records_[position_++];
    }

    void rewind() override { position_ = 0; }

private:
    std::vector<RawRecord> records_;
    std::size_t position_ = 0;
};

} // namespace xorcle
