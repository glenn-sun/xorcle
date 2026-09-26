#pragma once

/// @file proof_postprocess.h
/// @brief Turn a raw proof into an LRUP(⊕) proof: drop every derivation the
/// empty clause does not depend on, and prove each reason clause compaction has
/// reduced again as a line of its own.
///
/// Renumbering, and placing each clause's deletion after its last use, follow
/// from those.

#include <cstddef>
#include <ostream>
#include <span>
#include <vector>

#include "algebra/equation_repr.h"
#include "io/proof_raw.h"

namespace xorcle {

struct PostprocessSummary {
    std::size_t raw_learned = 0;      ///< Learned lines in the raw proof.
    std::size_t kept_learned = 0;     ///< Learned lines the proof keeps.
    std::size_t rewrite_lines = 0;    ///< Kept lines rewriting a compacted clause.
    std::size_t rewrite_unit_ids = 0; ///< Unit ids the kept rewrite lines cite.
    std::size_t deletion_lines = 0;
};

/// @brief Write the LRUP(⊕) proof for `source` to `out`.
///
/// A reason clause compaction has reduced is proved again, reduced, by a line
/// citing the units and the clause just before the first chain citing it, and
/// cited by that line from then on.
/// @param input_clause_rows The input clauses' rows, indexed by their 0-based
/// id. A compacted raw proof needs them, to say which units an input clause used
/// as a reason propagates modulo; an uncompacted one never reads them.
/// @pre `input_clause_rows.size() == num_input_clauses`.
/// @pre `source` holds exactly one `RawEmpty`, as its last learning record, and
/// is the raw proof the solver wrote for this instance.
PostprocessSummary postprocess_raw_proof(RawRecordSource& source, std::size_t num_vars,
                                         std::size_t num_input_clauses, EquationRepr repr,
                                         std::ostream& out,
                                         std::span<const std::vector<RawRow>> input_clause_rows);

} // namespace xorcle
