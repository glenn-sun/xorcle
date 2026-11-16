#pragma once

/// @file implication_graph.h
/// @brief The implication graph of the width-2 clauses, and the root units it
/// yields.

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "algebra/equation.h"
#include "algebra/equation_pool.h"
#include "algebra/equation_repr.h"
#include "algebra/row_echelon_matrix.h"
#include "core/clause_id.h"
#include "util/random.h"

namespace xorcle {

/// @brief When the solver reads the implication graph of the width-2 clauses,
/// and how much of it one run may read.
struct ImplicationGraphConfig {
    /// @brief Conflicts after a run or a skipped run before the next is due.
    std::size_t cadence;
    /// @brief The width-2 clauses new since the last run below which a due run is
    /// skipped; `0` skips none.
    std::size_t min_new_clauses;

    /// @brief Vertex pairs one run examines, and descendants one side's search
    /// keeps.
    std::size_t max_pairs;
    std::size_t max_descendants;
};

/// @brief No step of an `ImplicationGraphProofRecord`.
inline constexpr std::size_t no_proof_step = static_cast<std::size_t>(-1);

/// @brief One equation a run proved, and what proves it.
///
/// `equation` holds in every solution, so a proof of it is the proof of a
/// width-1 clause. The replays a step names are edges in the order the
/// descendant search reached the vertices they lead to, so following them from
/// the equation assumed derives each descendant from one already derived.
struct ImplicationGraphProofStep {
    /// @brief Which of the rule's four shapes derived the equation.
    enum class Kind {
        refuted,         ///< Replaying `edges` from the negation of `equation` reaches `0 = 1`.
        reaches_refuted, ///< The one edge leads from the negation of `equation` to what
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
    std::size_t edges_begin = 0;
    std::size_t edges_end = 0;
    std::size_t other_edges_begin = 0; ///< `both_sides` only: the negation's replay.
    std::size_t other_edges_end = 0;
};

/// @brief Every step of a run, with the edges they replay in one pool.
struct ImplicationGraphProofRecord {
    std::vector<ImplicationGraphProofStep> steps;
    std::vector<ClauseId> edges;
    /// @brief The step proving each of the run's units, in the order of its `units`.
    std::vector<std::size_t> unit_idx_to_step_idx;

    void clear() {
        steps.clear();
        edges.clear();
        unit_idx_to_step_idx.clear();
    }
};

/// @brief What one run of the graph did.
struct ImplicationGraphRun {
    std::size_t edges_visited = 0; ///< Edges the descendant searches traversed.
    std::size_t pairs_eligible = 0; ///< Vertex pairs with an out-edge on either side.
    std::size_t pairs_examined = 0; ///< Of those, the dirty ones the pair cap left room for.
    std::size_t refuted_sides = 0;  ///< Sides of the examined pairs whose span holds `0 = 1`.
    bool at_the_pair_cap = false;
    std::size_t truncated_sides = 0; ///< Sides the descendant cap cut a level short.
    double span_intersection_time_s = 0.0; ///< Of the run's own seconds.
    std::vector<Equation> units;
};

/// @brief The implication graph of the width-2 clauses.
///
/// A width-2 clause contributes the three nonzero equations of its linear
/// negation and the six implications among them,
/// since at most one of the three can hold. A vertex is one such equation; it
/// and its negation share a key, so `vertex ^ 1` is the negation and the edges
/// are closed under negating both ends.
///
/// A run builds the graph from the clause database and examines its dirty
/// pairs. A pair is dirty when a side of it reaches an edge of a clause first
/// read after the run that last examined the pair; a clause is read by the
/// first run that takes it, and is unread again once rewritten. Between runs
/// only that survives: per clause, the run that first read it and the runs
/// that last examined its three pairs.
class ImplicationGraph {
public:
    ImplicationGraph(EquationRepr repr, std::size_t num_vars,
                     const ImplicationGraphConfig& config);

    /// @brief Adopt a new active variable numbering, discarding the graph.
    void reconfigure(std::size_t num_vars);

    /// @brief Seed the stream the pair and descendant samples are drawn from.
    void set_base_seed(Seed base_seed);

    /// @brief Width-2 clauses, and the pairs of an equation and its negation the
    /// vertices make, as the last run held them.
    std::size_t num_width_2_clauses() const noexcept { return num_width_2_clauses_; }
    std::size_t num_vertex_pairs() const noexcept { return num_keys_; }

    /// @brief Look for units.
    ///
    /// A non-null `proof` is cleared and filled with a step per equation the run
    /// proved, and the step proving each of the run's `units`. A unit is offered
    /// by the run that finds it and by no later one.
    ///
    /// @pre `active_clause_ids` are ids of `clauses`, over the graph's variables, and
    /// the root trail is empty, its units substituted into the clauses.
    ImplicationGraphRun run(const std::vector<Clause>& clauses, ActiveClauseIds active_clause_ids,
                            ImplicationGraphProofRecord* proof = nullptr);

    /// @brief Make @p clause_id unread, its rows having changed.
    void clause_was_rewritten(ClauseId clause_id);

private:
    static constexpr std::size_t no_index = static_cast<std::size_t>(-1);

    /// @brief One implication, with the clause that stands behind it.
    struct Edge {
        std::size_t target;
        ClauseId clause_id;
    };

    /// @brief Point `triple_` at the three nonzero equations of @p clause's linear
    /// negation.
    /// @return Whether the clause is one the graph takes at all.
    bool reduce_triple(const Clause& clause);

    /// @brief The vertex of @p eqn, added if it is new.
    ///
    /// A key is whichever of an equation and its negation the graph saw first.
    /// @pre `eqn` is neither `0 = 0` nor `0 = 1`, and `hash` is `key_hash(eqn)`.
    std::size_t vertex_of(const Equation& eqn, std::uint32_t hash);

    /// @brief A hash an equation shares with its negation.
    static std::uint32_t key_hash(const Equation& eqn) noexcept;

    Equation equation_of(std::size_t vertex) const;

    /// @brief Add the three vertices of `triple_` and its six edges, reading the
    /// clause if it is unread.
    void add_clause(ClauseId clause_id);

    /// @brief Fill `key_is_dirty_`, in one search that visits every edge once.
    void mark_dirty_pairs();

    /// @brief Record this run as the one that last examined the sampled pairs,
    /// those with a side cut short included.
    void stamp_examined_pairs();

    /// @brief Stamp @p from and everything it reaches with @p mark, listing them
    /// in @p reached in breadth-first order.
    ///
    /// Whole distance levels go in while they fit under `max_descendants`; the
    /// level that would overrun it contributes a uniformly random subset filling
    /// the cap, and the search stops there. A non-null @p parent_clause is left
    /// holding the clause of the edge each of `reached[1]` onwards came in on.
    /// @return Whether that last level was cut short.
    bool mark_descendants(std::size_t from, std::vector<std::size_t>& stamp, std::size_t mark,
                          std::vector<std::size_t>& reached, std::size_t& edges_visited,
                          std::vector<ClauseId>* parent_clause);

    /// @brief A uniform sample of the dirty vertex pairs, at most `max_pairs` of
    /// them, for one run to examine.
    void sample_pairs(ImplicationGraphRun& summary);

    /// @brief Intersect the spans of the two sides of every sampled vertex pair,
    /// appending to @p units what the units before it leave independent.
    ///
    /// For a vertex `a`, `ImpliedBy(a)` is the span of `a` and its descendants,
    /// taken as the whole space once it holds `0 = 1`. Every equation in
    /// `ImpliedBy(a)` intersected with `ImpliedBy(not a)` holds in every solution,
    /// since exactly one of `a` and `not a` does.
    void walk_span_intersections(const std::vector<Clause>& clauses,
                                 ActiveClauseIds active_clause_ids, ImplicationGraphRun& summary,
                                 ImplicationGraphProofRecord* proof);

#ifndef NDEBUG
    /// @brief Whether examining every pair yields the units of @p run, in its
    /// order, or, where a width-1 clause is active, nothing outside the span of
    /// those units and such clauses; true also when `run` was at the pair cap or
    /// the descendant cap has cut a side in this run or an earlier one.
    bool full_examination_yields(const ImplicationGraphRun& run,
                                 const std::vector<Clause>& clauses,
                                 ActiveClauseIds active_clause_ids);

    /// @brief Whether some path from @p from to @p to has every edge backed by
    /// the width-2 clause it was built from.
    ///
    /// The clause is read again and reduced again, so this checks the graph
    /// against the database rather than against its own bookkeeping.
    bool implication_is_clause_backed(std::size_t from, std::size_t to,
                                      const std::vector<Clause>& clauses,
                                      ActiveClauseIds active_clause_ids);

    /// @brief Whether every edge the graph holds is backed by the width-2 clause
    /// it was built from, the clause read and reduced again.
    bool all_edges_are_clause_backed(const std::vector<Clause>& clauses,
                                    ActiveClauseIds active_clause_ids);
#endif

    EquationRepr repr_;
    std::size_t num_vars_;
    std::size_t max_pairs_;
    std::size_t max_descendants_;
    Rng rng_;

    /// @brief The keys in `[0, num_keys_)`; the equations past them are kept for
    /// the next run to overwrite.
    std::vector<Equation> key_eqn_;
    std::size_t num_keys_ = 0;
    std::vector<std::uint32_t> key_hash_; ///< `key_hash()` of each key.

    /// @brief An open-addressing table of key index plus one, 0 for an empty
    /// slot, a power of two in size and probed linearly from `slot_of()`.
    std::vector<std::uint32_t> key_slots_;
    unsigned key_slot_shift_ = 0;
    std::size_t slot_of(std::uint32_t hash) const noexcept;

    /// @brief Forget every key, keeping the equations.
    void clear_keys();
    std::vector<std::vector<Edge>> edges_; ///< Indexed by vertex.

    std::size_t num_width_2_clauses_ = 0;

    /// @brief What a clause keeps between runs; a run is named by its count from
    /// 1, and 0 is no run.
    ///
    /// A renumbering keeps the variables in order, so a clause not rewritten
    /// keeps its `triple_`.
    struct ClauseRuns {
        std::uint32_t first_read = 0;
        std::array<std::uint32_t, 3> last_examined{}; ///< The pair of each of `triple_`.
        /// @brief `key_hash()` of each of `triple_` under the variable numbering
        /// `hash_numbering`; 0 is no numbering.
        std::array<std::uint32_t, 3> hash{};
        std::uint32_t hash_numbering = 0;
    };
    std::vector<ClauseRuns> clause_runs_; ///< Indexed by clause id.
    std::uint32_t num_runs_ = 0;
    std::uint32_t numbering_ = 1; ///< Counts the variable numberings adopted.

    /// @brief A clause of the graph, with the vertices of its `triple_`.
    struct GraphClause {
        ClauseId clause_id;
        std::array<std::size_t, 3> vertices;
    };
    std::vector<GraphClause> graph_clauses_;
    std::vector<std::uint32_t> key_last_examined_;
    std::vector<std::uint8_t> key_is_dirty_;

    /// @brief By vertex, the latest first read among the clauses whose edges its
    /// negation reaches.
    std::vector<std::uint32_t> latest_first_read_;
    std::vector<std::size_t> graph_clause_order_;
    std::vector<std::size_t> order_begin_;
    std::vector<std::size_t> search_queue_;
#ifndef NDEBUG
    bool a_side_was_truncated_ = false;
#endif

    /// @brief The rows `reduce_triple()` last took and their sum.
    std::array<const Equation*, 3> triple_{};
    Equation row_sum_;

    /// @brief The even vertex of each pair the walk is to examine.
    std::vector<std::size_t> sampled_pairs_;

    std::vector<std::size_t> descendant_stamp_;
    std::vector<std::size_t> other_descendant_stamp_;
    std::size_t next_mark_ = 0;
    std::vector<std::size_t> descendants_;
    std::vector<std::size_t> other_descendants_;

    /// @brief The edge each descendant came in on, filled only while a proof is
    /// being recorded.
    std::vector<ClauseId> parent_clauses_;
    std::vector<ClauseId> other_parent_clauses_;

    /// @brief Rows the walk's spans gave back, reused so building a span allocates no rows.
    EquationPool spare_rows_;
};

} // namespace xorcle
