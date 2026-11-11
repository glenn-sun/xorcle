#pragma once

/// @file gate_solver.h
/// @brief BVE's gate search: whether clauses are unsatisfiable together, and a core of
/// them if so, by their truth table or by a small CDCL(⊕) solver.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "algebra/dense/dense_gf2_vector.h"
#include "util/random.h"

namespace xorcle {

/// @brief How a gate search shrinks its first core.
enum class BveCoreShrinking {
    none, ///< Keep the first core.
    repeat ///< Solve again on the core until it stops shrinking.
};

/// @brief The derivation of the empty clause from a gate, by LRUP(⊕) lines.
///
/// A reason below the number of clauses given names that clause; one at or above names
/// the lemma at its place less that number. Each lemma follows from its reasons by
/// unit propagation in their order, and the empty clause from `reasons`.
struct GateRefutation {
    struct Lemma {
        std::vector<DenseGF2Vector> rows; ///< Its linear negation.
        std::vector<std::uint32_t> reasons;
    };
    std::vector<Lemma> lemmas;
    std::vector<std::uint32_t> reasons;
};

/// @brief What one gate search found.
struct GateSearch {
    /// @brief The clauses of the gate, ascending; nothing if the clauses are
    /// satisfiable together.
    std::optional<std::vector<std::uint32_t>> core;
    /// @brief Of a gate the gate solver found, from the solve that found it, citing only
    /// the gate's clauses; empty for a table's gate.
    GateRefutation refutation;
    bool by_table = false;
};

/// @brief Whether @p clauses are unsatisfiable together, and if so a core of them
/// that @p shrinking shrinks.
///
/// Each solve is CDCL(⊕) as in arXiv:2605.15002, without restarts or deletion.
/// Propagation is complete: a cyclic selector visits the clauses, each tracking two
/// of its rows. A decision is drawn uniformly from the intersection, modulo the
/// trail, of the open clauses' spans in move-to-front order, skipping a clause the
/// intersection would miss, and the selector visits the clauses intersected first.
/// The core is the input clauses the final `0 = 1` derives from. A solve whose
/// conflict number @p max_conflicts is above the root stops there and finds nothing.
///
/// @param clauses Each clause in its linear negation, over @p num_bits bits, the
/// last the constant.
/// @param rng Drawn from for the clause order of each solve and the decisions.
/// @pre `num_bits >= 1` and `max_conflicts >= 1`; each clause's rows have
/// `num_bits` bits and are independent, and their span does not hold `0 = 1`.
GateSearch search_gate(const std::vector<std::vector<DenseGF2Vector>>& clauses,
                       std::size_t num_bits, BveCoreShrinking shrinking,
                       std::size_t max_conflicts, Rng& rng);

/// @brief The truth table of the points the clauses added so far forbid, with its
/// buffers kept from one gate search to the next.
class GateTable {
public:
    /// @brief A minimal set of @p clauses unsatisfiable together as the core, or no core
    /// if they are satisfiable together.
    ///
    /// The clauses are added fewest rows first, equal ones in an order drawn uniformly,
    /// until those added are unsatisfiable together; each one the others then cover
    /// without is taken out, tried in the order added.
    /// @param clauses As `search_gate()` takes them.
    /// @pre As `search_gate()`'s, and `num_bits <= 70`.
    GateSearch search(const std::vector<std::vector<DenseGF2Vector>>& clauses, std::size_t num_bits, Rng& rng);

private:
    /// @brief Add the points @p clause forbids; whether every point is now forbidden.
    bool add(const std::vector<DenseGF2Vector>& clause);
    std::vector<std::uint8_t> minimal_cover() const;

    std::size_t num_coordinates_ = 0;
    std::size_t num_words_ = 0;
    std::size_t num_added_ = 0;
    std::vector<std::uint64_t> forbidden_;
    /// @brief The points each clause added forbids, `num_words_` words a clause.
    std::vector<std::uint64_t> forbidden_by_;
};

/// @brief A gate among @p clauses: by @p table when `clauses² × max(1, 2^(rank - 6))`, the
/// table's cost in clause pairs times words with `rank = num_bits - 1`, is at most
/// @p max_table_cost, and otherwise by `search_gate()`.
/// @pre As `search_gate()`'s.
GateSearch search_gate_hybrid(const std::vector<std::vector<DenseGF2Vector>>& clauses,
                              std::size_t num_bits, std::size_t max_table_cost,
                              BveCoreShrinking shrinking, std::size_t max_conflicts,
                              GateTable& table, Rng& rng);

/// @brief The refutation of @p gate, by the gate solver without a conflict cap.
///
/// It cites the clauses by their place in @p clauses.
/// @pre @p gate is ascending and its clauses among @p clauses are unsatisfiable
/// together, and otherwise as `search_gate()`'s.
GateRefutation refute_gate(const std::vector<std::vector<DenseGF2Vector>>& clauses,
                           std::size_t num_bits, const std::vector<std::uint32_t>& gate, Rng& rng);

} // namespace xorcle
