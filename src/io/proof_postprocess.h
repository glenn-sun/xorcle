#pragma once

/// @file proof_postprocess.h
/// @brief Turn a raw proof into an LRUP(⊕) proof: derive the units compaction
/// eliminated, drop every derivation the empty clause does not depend on,
/// renumber, and delete each clause after its last use.

#include <cstddef>
#include <ostream>
#include <span>
#include <vector>

#include "algebra/equation_repr.h"
#include "io/proof_raw.h"

namespace xorcle {

struct PostprocessSummary {
    std::size_t raw_learned = 0;    ///< Learned lines in the raw proof.
    std::size_t kept_learned = 0;   ///< Learned lines the proof keeps.
    std::size_t unit_lines = 0;     ///< Unit lines added for compaction.
    std::size_t inserted_units = 0; ///< Unit ids inserted into reason lists.
    std::size_t deletion_lines = 0;
};

/// @brief Write the LRUP(⊕) proof for `source` to `out`.
/// @param input_clause_rows The input clauses' rows, indexed by their 0-based
/// id. A compacted raw proof needs them, to say which units an input clause used
/// as a reason propagates modulo; an uncompacted one never reads them.
/// @pre `input_clause_rows.size() == num_input_clauses`.
/// @pre `source` holds exactly one `RawEmpty`, as its last learning record.
/// @throws std::runtime_error On a raw proof that cannot be turned into a proof.
PostprocessSummary postprocess_raw_proof(RawRecordSource& source, std::size_t num_vars,
                                         std::size_t num_input_clauses, EquationRepr repr,
                                         std::ostream& out,
                                         std::span<const std::vector<RawRow>> input_clause_rows);

} // namespace xorcle
