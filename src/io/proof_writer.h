#pragma once

/// @file proof_writer.h
/// @brief The LRUP(⊕) proof the solver writes as it runs.

#include <cstddef>
#include <ostream>
#include <span>
#include <vector>

#include "algebra/equation.h"
#include "algebra/row_echelon_matrix.h"
#include "core/clause_id.h"
#include "heuristics/bve/elimination.h"

namespace xorcle {

/// @brief One equation over the original variables: ascending bit indices, with the
/// variable count standing for the constant bit, as `Equation::bits()` reports it.
using ProofRow = std::vector<std::size_t>;

/// @brief One row lifted out of the active numbering into the original one.
///
/// Bit `i` of `row` is variable `active_var_to_original[i]`, and its constant
/// bit `active_var_to_original.size()` is `original_num_vars`.
ProofRow lift_row(const Equation& row, std::span<const std::size_t> active_var_to_original,
                  std::size_t original_num_vars);

/// @brief A BVE row lifted out of the active numbering into the original one, as the
/// other overload lifts an equation.
ProofRow lift_row(const VarListEquation& row, std::span<const std::size_t> active_var_to_original,
                  std::size_t original_num_vars);

/// @brief Writes an LRUP(⊕) proof line by line, every row over the original variables.
///
/// The solver numbers clauses from 0, the input clauses first in file order; a line
/// carries its clause's number plus one, as the format numbers input clauses from 1.
class ProofWriter {
public:
    ProofWriter(std::ostream& out, std::size_t num_vars) : out_(out), num_vars_(num_vars) {}

    /// @brief The original variable count, which every row is written over.
    std::size_t num_vars() const noexcept { return num_vars_; }

    /// @brief Write a learned clause in its linear negation, lifting every row out of the
    /// active numbering.
    /// @pre `id` exceeds every id written before, and `reasons` is in replay order.
    void write_learned(ClauseId id, const Clause& clause,
                       std::span<const std::size_t> active_var_to_original,
                       const std::vector<ClauseId>& reasons);

    /// @brief Write a learned clause whose rows are already over the original variables.
    /// @pre As the other overload's.
    void write_learned(ClauseId id, std::span<const ProofRow> rows,
                       const std::vector<ClauseId>& reasons);

    /// @brief Write the empty clause.
    /// @pre As `write_learned()`'s.
    void write_empty(ClauseId id, const std::vector<ClauseId>& reasons);

    /// @brief Write the deletion of clauses no later line names.
    /// @pre `ids` is non-empty.
    void write_deleted(const std::vector<ClauseId>& ids);

private:
    void write_row(const ProofRow& row);
    void write_reasons(const std::vector<ClauseId>& reasons);

    std::ostream& out_;
    std::size_t num_vars_;
};

} // namespace xorcle
