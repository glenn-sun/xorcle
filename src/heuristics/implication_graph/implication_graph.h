#pragma once

/// @file implication_graph.h
/// @brief The implication graph of the width-2 clauses, and the root units it
/// yields.

#include <array>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "algebra/equation.h"
#include "algebra/equation_repr.h"
#include "algebra/row_echelon_matrix.h"
#include "core/clause_id.h"
#include "util/random.h"

namespace xorcle {

/// @brief When the solver reads the implication graph of the width-2 clauses,
/// and how much of it one run may read.
struct ImplicationGraphConfig {
    bool enabled = true;

    /// @brief Clauses that reached width at most 2 since the last run, and, as a
    /// fallback, conflicts since it, either of which makes a run due.
    std::size_t clause_cadence = 50;
    std::size_t cadence = 1000;

    /// @brief Vertex pairs one run examines, and descendants one side's search
    /// keeps.
    std::size_t max_pairs = 10000;
    std::size_t max_descendants = 100;

    bool operator==(const ImplicationGraphConfig&) const = default;
};

/// @brief No step of an `ImplicationGraphProofRecord`.
inline constexpr std::size_t no_proof_step = static_cast<std::size_t>(-1);

/// @brief One equation a run proved, and what proves it.
///
/// `equation` holds in every solution, so a proof of it is the proof of a
/// width-1 clause. The replays a step names are arcs in the order the
/// descendant search reached the vertices they lead to, so following them from
/// the equation assumed derives each descendant from one already derived.
struct ImplicationGraphProofStep {
    /// @brief Which of the rule's four shapes derived the equation.
    enum class Kind {
        refuted,         ///< Replaying `arcs` from the negation of `equation` reaches `0 = 1`.
        reaches_refuted, ///< The one arc leads from the negation of `equation` to what
                         ///< `source` refutes.
        one_side,        ///< `equation` is in the span of the replay from what `source`
                         ///< proves, the negation of a refuted equation.
        both_sides,      ///< `equation` is in the span of the replay from `assumed` and in
                         ///< the span of the replay from its negation.
    };

    Kind kind;
    Equation equation;
    Equation assumed;   ///< `both_sides` only; the other kinds read `equation` and `source`.
    std::size_t source = no_proof_step;
    std::size_t arcs_begin = 0;
    std::size_t arcs_end = 0;
    std::size_t other_arcs_begin = 0; ///< `both_sides` only: the negation's replay.
    std::size_t other_arcs_end = 0;
};

/// @brief Every step of a run, with the arcs they replay in one pool.
struct ImplicationGraphProofRecord {
    std::vector<ImplicationGraphProofStep> steps;
    std::vector<ClauseId> arcs;
    /// @brief The step proving each of the run's units, in the order of its `units`.
    std::vector<std::size_t> unit_idx_to_step_idx;

    void clear() {
        steps.clear();
        arcs.clear();
        unit_idx_to_step_idx.clear();
    }
};

/// @brief What one run of the graph did.
struct ImplicationGraphRun {
    bool did_work = false;         ///< False when there was no graph to read.
    std::size_t edges_visited = 0; ///< Arcs the descendant searches traversed.
    std::size_t pairs_eligible = 0; ///< Vertex pairs with an out-arc on either side.
    std::size_t pairs_examined = 0; ///< Of those, the ones the pair cap left room for.
    std::size_t refuted_sides = 0;  ///< Sides of the examined pairs whose span holds `0 = 1`.
    bool at_the_pair_cap = false;
    std::size_t truncated_sides = 0; ///< Sides the descendant cap cut a level short.
    double span_intersection_time_s = 0.0; ///< Of the run's own seconds.
    std::vector<Equation> units; ///< Units the root trail did not already have.
};

/// @brief The implication graph of the width-2 clauses.
///
/// A width-2 clause contributes the three nonzero equations of its linear
/// negation, reduced by the root trail, and the six implications among them,
/// since at most one of the three can hold. A vertex is one such equation; it
/// and its negation share a key, so `vertex ^ 1` is the negation and the arcs
/// are closed under negating both ends.
///
/// A run builds the graph from the clause database and reads it; nothing but
/// scratch survives to the next one.
class ImplicationGraph {
public:
    ImplicationGraph(EquationRepr repr, std::size_t num_vars,
                     const ImplicationGraphConfig& config = {});

    /// @brief Adopt a new active variable numbering, discarding the graph.
    void reconfigure(std::size_t num_vars);

    /// @brief Seed the stream the pair and descendant samples are drawn from.
    void set_base_seed(Seed base_seed);

    /// @brief Width-2 clauses, and the pairs of an equation and its negation the
    /// vertices make, as the last run held them.
    std::size_t num_width_2_clauses() const noexcept { return num_width_2_clauses_; }
    std::size_t num_vertex_pairs() const noexcept { return key_eqn_.size(); }

    /// @brief Look for units the root trail does not already have.
    ///
    /// A non-null `proof` is cleared and filled with a step per equation the run
    /// proved, and the step proving each of the run's `units`.
    ///
    /// @pre `active_clause_ids` are ids of `clauses`, and every clause shares
    /// `root_span`'s representation and column count.
    ImplicationGraphRun run(const std::vector<Clause>& clauses, ActiveClauseIds active_clause_ids,
                            const RowSpace& root_span,
                            ImplicationGraphProofRecord* proof = nullptr);

private:
    static constexpr std::size_t no_index = static_cast<std::size_t>(-1);

    /// @brief One implication, with the clause that stands behind it.
    struct Arc {
        std::size_t target;
        ClauseId clause_id;
    };

    /// @brief Reduce @p clause by the root trail into `triple_`: the three
    /// nonzero equations of its linear negation.
    /// @return Whether the clause is one the graph takes at all.
    bool reduce_triple(const Clause& clause, const RowSpace& root_span);

    /// @brief The vertex of @p eqn, added if it is new.
    ///
    /// A key is whichever of an equation and its negation the graph saw first,
    /// so a lookup that misses tries the other before allocating.
    /// @pre `eqn` is neither `0 = 0` nor `0 = 1`.
    std::size_t vertex_of(const Equation& eqn);

    Equation equation_of(std::size_t vertex) const;

    /// @brief An equation over @p num_vars variables with any contents, taken
    /// from `spare_rows_` when it has one.
    Equation spare_row(std::size_t num_vars);

    /// @brief Empty @p span, keeping its rows in `spare_rows_`.
    void clear_span(RowSpace& span);

    /// @brief The three vertices of `triple_`, with its six arcs added.
    void add_clause(ClauseId clause_id);

    /// @brief Stamp @p from and everything it reaches with @p mark, listing them
    /// in @p reached in breadth-first order.
    ///
    /// Whole distance levels go in while they fit under `max_descendants`; the
    /// level that would overrun it contributes a uniformly random subset filling
    /// the cap, and the search stops there. A non-null @p parent_clause is left
    /// holding the clause of the arc each of `reached[1]` onwards came in on.
    /// @return Whether that last level was cut short.
    bool mark_descendants(std::size_t from, std::vector<std::size_t>& stamp, std::size_t mark,
                          std::vector<std::size_t>& reached, std::size_t& edges_visited,
                          std::vector<ClauseId>* parent_clause);

    /// @brief A uniform sample of the vertex pairs with an out-arc on either side,
    /// at most `max_pairs` of them, for one run to examine.
    void sample_pairs(ImplicationGraphRun& summary);

    /// @brief Intersect the spans of the two sides of every sampled vertex pair,
    /// appending to @p units what the root span and the units before it leave
    /// independent.
    ///
    /// For a vertex `a`, `T_a` is the span of `a` and everything `a` reaches,
    /// taken as the whole space once it holds `0 = 1`. Every equation in `T_a`
    /// intersected with `T_not_a` holds in every solution, since exactly one of
    /// `a` and `not a` does.
    void walk_span_intersections(const RowSpace& root_span, const std::vector<Clause>& clauses,
                                 ActiveClauseIds active_clause_ids, ImplicationGraphRun& summary,
                                 ImplicationGraphProofRecord* proof);

#ifndef NDEBUG
    /// @brief Whether some path from @p from to @p to has every arc backed by
    /// the width-2 clause it was built from.
    ///
    /// The clause is read again and reduced again, so this checks the graph
    /// against the database rather than against its own bookkeeping.
    bool implication_is_clause_backed(std::size_t from, std::size_t to,
                                      const std::vector<Clause>& clauses,
                                      ActiveClauseIds active_clause_ids, const RowSpace& root_span);

    /// @brief Whether every arc the graph holds is backed by the width-2 clause
    /// it was built from, the clause read and reduced again.
    bool all_arcs_are_clause_backed(const std::vector<Clause>& clauses,
                                    ActiveClauseIds active_clause_ids, const RowSpace& root_span);
#endif

    EquationRepr repr_;
    std::size_t num_vars_;
    std::size_t max_pairs_;
    std::size_t max_descendants_;
    Equation false_eqn_;
    Rng rng_;

    /// @brief Key to its index; the variable bits alone, so the map's nodes hold
    /// the one copy of each equation `key_eqn_` then points at.
    std::unordered_map<Equation, std::size_t, EquationHash> key_of_;
    std::vector<const Equation*> key_eqn_;
    std::vector<std::vector<Arc>> arcs_; ///< Indexed by vertex.

    std::size_t num_width_2_clauses_ = 0;

    /// @brief Reused across every clause a run reads, so reading one allocates
    /// nothing but the key of a vertex the graph has not seen before.
    Equation lookup_scratch_;
    RowSpace reduced_;
    Equation row_scratch_;
    std::array<Equation, 3> triple_;

    /// @brief The even vertex of each pair the walk is to examine.
    std::vector<std::size_t> sampled_pairs_;

    std::vector<std::size_t> descendant_stamp_;
    std::vector<std::size_t> other_descendant_stamp_;
    std::size_t next_mark_ = 0;
    std::vector<std::size_t> descendants_;
    std::vector<std::size_t> other_descendants_;

    /// @brief The arc each descendant came in on, filled only while a proof is
    /// being recorded.
    std::vector<ClauseId> parent_clauses_;
    std::vector<ClauseId> other_parent_clauses_;

    /// @brief Rows the walk's spans gave back, of any bit count, reused so building
    /// a span allocates no rows.
    std::vector<Equation> spare_rows_;
};

} // namespace xorcle
