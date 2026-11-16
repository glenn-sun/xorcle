#include "heuristics/implication_graph/implication_graph.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <limits>
#include <utility>

namespace xorcle {

ImplicationGraph::ImplicationGraph(const EquationRepr repr, const std::size_t num_vars,
                                   const ImplicationGraphConfig& config)
    : repr_(repr),
      num_vars_(num_vars),
      max_pairs_(config.max_pairs),
      max_descendants_(config.max_descendants),
      rng_(make_rng(default_seed, RngComponent::implication_graph_sample)),
      row_sum_(repr, num_vars),
      spare_rows_(repr, num_vars + 1) {
    assert(max_pairs_ != 0 && max_descendants_ != 0 && "a run must be allowed one of each");
}

void ImplicationGraph::set_base_seed(const Seed base_seed) {
    rng_ = make_rng(base_seed, RngComponent::implication_graph_sample);
}

void ImplicationGraph::reconfigure(const std::size_t num_vars) {
    if (num_vars != num_vars_) {
        key_eqn_.clear();
        ++numbering_;
    }
    num_vars_ = num_vars;
    row_sum_.resize(num_vars);
    spare_rows_.clear(num_vars + 1);
    clear_keys();
    key_last_examined_.clear();
    graph_clauses_.clear();
    for (std::vector<Edge>& edges : edges_) {
        edges.clear();
    }
    num_width_2_clauses_ = 0;
}

bool ImplicationGraph::reduce_triple(const Clause& clause) {
    // A clause spanning `0 = 1` is tautological.
    if (clause.rows() != 2 || clause.pivot_col_idxs().test(num_vars_)) {
        return false;
    }
    row_sum_.assign_from(clause.row(0));
    row_sum_.plus_assign(clause.row(1));
    triple_ = {&clause.row(0), &clause.row(1), &row_sum_};
    return true;
}

std::uint32_t ImplicationGraph::key_hash(const Equation& eqn) noexcept {
    const std::uint64_t hash = eqn.variables_hash();
    return static_cast<std::uint32_t>(hash ^ (hash >> 32));
}

std::size_t ImplicationGraph::slot_of(const std::uint32_t hash) const noexcept {
    // Fibonacci hashing: the product's high bits depend on every bit of the hash.
    return static_cast<std::size_t>((static_cast<std::uint64_t>(hash) * 0x9E3779B97F4A7C15ULL) >>
                                    key_slot_shift_);
}

void ImplicationGraph::clear_keys() {
    num_keys_ = 0;
    key_hash_.clear();
    if (key_slots_.empty()) {
        key_slots_.resize(64);
        key_slot_shift_ = 64 - 6;
    }
    std::fill(key_slots_.begin(), key_slots_.end(), 0);
}

std::size_t ImplicationGraph::vertex_of(const Equation& eqn, const std::uint32_t hash) {
    assert(!eqn.is_eq_true() && !eqn.is_eq_false() &&
           "an implication graph vertex carries no variable");
    assert(hash == key_hash(eqn) && "a clause kept the hash of other rows");

    const std::size_t slot_mask = key_slots_.size() - 1;
    std::size_t slot = slot_of(hash);
    for (; key_slots_[slot] != 0; slot = (slot + 1) & slot_mask) {
        const std::size_t key = key_slots_[slot] - 1;
        if (key_hash_[key] == hash && key_eqn_[key].equal_variables(eqn)) {
            return 2 * key + (key_eqn_[key] == eqn ? 0 : 1);
        }
    }

    const std::size_t key = num_keys_++;
    assert(key < std::numeric_limits<std::uint32_t>::max() && "the keys outgrew their table");
    if (key == key_eqn_.size()) {
        key_eqn_.push_back(eqn);
    } else {
        key_eqn_[key].assign_from(eqn);
    }
    key_hash_.push_back(hash);
    key_slots_[slot] = static_cast<std::uint32_t>(key + 1);
    if (2 * num_keys_ > key_slots_.size()) {
        key_slots_.assign(2 * key_slots_.size(), 0);
        --key_slot_shift_;
        for (std::size_t i = 0; i < num_keys_; ++i) {
            std::size_t at = slot_of(key_hash_[i]);
            while (key_slots_[at] != 0) {
                at = (at + 1) & (key_slots_.size() - 1);
            }
            key_slots_[at] = static_cast<std::uint32_t>(i + 1);
        }
    }
    if (edges_.size() < 2 * num_keys_) {
        edges_.resize(2 * num_keys_);
    }
    descendant_stamp_.resize(2 * num_keys_, 0);
    other_descendant_stamp_.resize(2 * num_keys_, 0);
    key_last_examined_.push_back(0);
    return 2 * key;
}

Equation ImplicationGraph::equation_of(const std::size_t vertex) const {
    Equation eqn = key_eqn_[vertex / 2];
    if ((vertex & 1) != 0) {
        eqn.negate();
    }
    return eqn;
}

void ImplicationGraph::clause_was_rewritten(const ClauseId clause_id) {
    if (clause_id < clause_runs_.size()) {
        clause_runs_[clause_id] = ClauseRuns{};
    }
}

void ImplicationGraph::add_clause(const ClauseId clause_id) {
    ClauseRuns& runs = clause_runs_[clause_id];
    if (runs.hash_numbering != numbering_) {
        for (std::size_t i = 0; i < 3; ++i) {
            runs.hash[i] = key_hash(*triple_[i]);
        }
        runs.hash_numbering = numbering_;
    }
    const std::array<std::size_t, 3> vertices{vertex_of(*triple_[0], runs.hash[0]),
                                              vertex_of(*triple_[1], runs.hash[1]),
                                              vertex_of(*triple_[2], runs.hash[2])};
    if (runs.first_read == 0) {
        runs.first_read = num_runs_;
    }
    // Every clause of a pair that the run examining it took still names that
    // run, so the latest any of them names is the pair's last.
    for (std::size_t i = 0; i < 3; ++i) {
        std::uint32_t& last_examined = key_last_examined_[vertices[i] / 2];
        last_examined = std::max(last_examined, runs.last_examined[i]);
    }
    graph_clauses_.push_back(GraphClause{clause_id, vertices});
    // At most one of the three holds, so each refutes the other two.
    for (const std::size_t from : vertices) {
        for (const std::size_t to : vertices) {
            if (from != to) {
                edges_[from].push_back(Edge{to ^ 1, clause_id});
            }
        }
    }
    ++num_width_2_clauses_;
}

bool ImplicationGraph::mark_descendants(const std::size_t from, std::vector<std::size_t>& stamp,
                                        const std::size_t mark,
                                        std::vector<std::size_t>& reached,
                                        std::size_t& edges_visited,
                                        std::vector<ClauseId>* parent_clause) {
    reached.assign(1, from);
    stamp[from] = mark;
    if (parent_clause != nullptr) {
        parent_clause->clear();
    }
    for (std::size_t level_begin = 0; level_begin < reached.size();) {
        const std::size_t next_begin = reached.size();
        for (std::size_t head = level_begin; head < next_begin; ++head) {
            for (const Edge& edge : edges_[reached[head]]) {
                ++edges_visited;
                if (stamp[edge.target] != mark) {
                    stamp[edge.target] = mark;
                    reached.push_back(edge.target);
                    if (parent_clause != nullptr) {
                        parent_clause->push_back(edge.clause_id);
                    }
                }
            }
        }
        if (reached.size() > max_descendants_) {
            // Any subset of the descendants spans a subspace of the full span,
            // so a unit found in the intersection still holds and a refuted side
            // is still refuted.
            for (std::size_t i = next_begin; i < max_descendants_; ++i) {
                const std::size_t pick = i + uniform_below(rng_, reached.size() - i);
                std::swap(reached[i], reached[pick]);
                if (parent_clause != nullptr) {
                    std::swap((*parent_clause)[i - 1], (*parent_clause)[pick - 1]);
                }
            }
            reached.resize(max_descendants_);
            if (parent_clause != nullptr) {
                parent_clause->resize(max_descendants_ - 1);
            }
            return true;
        }
        level_begin = next_begin;
    }
    return false;
}

void ImplicationGraph::mark_dirty_pairs() {
    const std::size_t num_vertices = 2 * num_keys_;
    const auto first_read = [this](const std::size_t i) {
        return clause_runs_[graph_clauses_[i].clause_id].first_read;
    };
    // A counting sort by first read, latest first: `order_begin_[r]` ends as
    // where the clauses first read by run `r` begin.
    order_begin_.assign(num_runs_ + 2, 0);
    for (std::size_t i = 0; i < graph_clauses_.size(); ++i) {
        ++order_begin_[first_read(i)];
    }
    std::size_t num_later = 0;
    for (std::size_t run = num_runs_ + 1; run-- > 0;) {
        const std::size_t count = order_begin_[run];
        order_begin_[run] = num_later;
        num_later += count;
    }
    graph_clause_order_.resize(graph_clauses_.size());
    for (std::size_t i = 0; i < graph_clauses_.size(); ++i) {
        graph_clause_order_[order_begin_[first_read(i)]++] = i;
    }

    // An edge `u -> w` has the mirror `not w -> not u`, so what reaches a vertex
    // of a clause is the negation of what that vertex's negation reaches. The
    // clauses go in from the latest read, so the first to reach a vertex is the
    // latest that does.
    latest_first_read_.assign(num_vertices, 0);
    search_queue_.clear();
    std::size_t head = 0;
    for (const std::size_t i : graph_clause_order_) {
        const std::uint32_t read = first_read(i);
        for (const std::size_t vertex : graph_clauses_[i].vertices) {
            if (latest_first_read_[vertex ^ 1] == 0) {
                latest_first_read_[vertex ^ 1] = read;
                search_queue_.push_back(vertex ^ 1);
            }
        }
        for (; head < search_queue_.size(); ++head) {
            for (const Edge& edge : edges_[search_queue_[head]]) {
                if (latest_first_read_[edge.target] == 0) {
                    latest_first_read_[edge.target] = read;
                    search_queue_.push_back(edge.target);
                }
            }
        }
    }

    key_is_dirty_.resize(num_keys_);
    for (std::size_t key = 0; key < num_keys_; ++key) {
        key_is_dirty_[key] =
            std::max(latest_first_read_[2 * key], latest_first_read_[2 * key + 1]) >
                    key_last_examined_[key]
                ? 1
                : 0;
    }
}

void ImplicationGraph::stamp_examined_pairs() {
    for (const std::size_t vertex : sampled_pairs_) {
        key_last_examined_[vertex / 2] = num_runs_;
    }
    for (const GraphClause& graph_clause : graph_clauses_) {
        ClauseRuns& runs = clause_runs_[graph_clause.clause_id];
        for (std::size_t i = 0; i < 3; ++i) {
            runs.last_examined[i] = key_last_examined_[graph_clause.vertices[i] / 2];
        }
    }
}

void ImplicationGraph::sample_pairs(ImplicationGraphRun& summary) {
    sampled_pairs_.clear();
    const std::size_t num_vertices = 2 * num_keys_;
    for (std::size_t vertex = 0; vertex < num_vertices; vertex += 2) {
        if (!edges_[vertex].empty() || !edges_[vertex + 1].empty()) {
            ++summary.pairs_eligible;
            if (key_is_dirty_[vertex / 2] != 0) {
                sampled_pairs_.push_back(vertex);
            }
        }
    }
    if (sampled_pairs_.size() <= max_pairs_) {
        return;
    }
    // Partial Fisher-Yates: the first `max_pairs_` places end up holding a
    // uniformly random subset of the dirty pairs.
    for (std::size_t i = 0; i < max_pairs_; ++i) {
        const std::size_t pick = i + uniform_below(rng_, sampled_pairs_.size() - i);
        std::swap(sampled_pairs_[i], sampled_pairs_[pick]);
    }
    sampled_pairs_.resize(max_pairs_);
    summary.at_the_pair_cap = true;
}

ImplicationGraphRun ImplicationGraph::run(const std::vector<Clause>& clauses,
                                          ActiveClauseIds active_clause_ids,
                                          ImplicationGraphProofRecord* proof) {
    ImplicationGraphRun summary;
    if (proof != nullptr) {
        proof->clear();
    }

    clear_keys();
    for (std::vector<Edge>& edges : edges_) {
        edges.clear();
    }
    num_width_2_clauses_ = 0;
    graph_clauses_.clear();
    key_last_examined_.clear();

    clause_runs_.resize(clauses.size());
    ++num_runs_;

    for (const ClauseId clause_id : active_clause_ids) {
        if (reduce_triple(clauses[clause_id])) {
            add_clause(clause_id);
        }
    }

    if (num_keys_ != 0) {
        assert(all_edges_are_clause_backed(clauses, active_clause_ids) &&
               "an edge of the implication graph is not witnessed by its clause");
        const auto start = std::chrono::steady_clock::now();
        mark_dirty_pairs();
        sample_pairs(summary);
        walk_span_intersections(clauses, active_clause_ids, summary, proof);
        stamp_examined_pairs();
        const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start;
        summary.span_intersection_time_s += elapsed.count();
        assert(full_examination_yields(summary, clauses, active_clause_ids) &&
               "a clean pair held a unit");
    }
    assert((proof == nullptr ||
            std::ranges::equal(summary.units, proof->unit_idx_to_step_idx,
                               [proof](const Equation& unit, const std::size_t step_idx) {
                                   return unit == proof->steps[step_idx].equation;
                               })) &&
           "a unit is not the equation of the step recorded as proving it");
    return summary;
}

#ifndef NDEBUG
bool ImplicationGraph::implication_is_clause_backed(
    const std::size_t from, const std::size_t to, const std::vector<Clause>& clauses,
    ActiveClauseIds active_clause_ids) {
    const std::size_t num_vertices = 2 * num_keys_;
    std::vector<std::size_t> parent(num_vertices, no_index);
    std::vector<std::size_t> parent_edge(num_vertices, no_index);
    std::vector<std::uint8_t> seen(num_vertices, 0);
    std::vector<std::size_t> queue{from};
    seen[from] = 1;
    for (std::size_t head = 0; head < queue.size() && seen[to] == 0; ++head) {
        const std::size_t vertex = queue[head];
        for (std::size_t i = 0; i < edges_[vertex].size(); ++i) {
            const std::size_t target = edges_[vertex][i].target;
            if (seen[target] == 0) {
                seen[target] = 1;
                parent[target] = vertex;
                parent_edge[target] = i;
                queue.push_back(target);
            }
        }
    }
    if (seen[to] == 0) {
        return false;
    }

    for (std::size_t vertex = to; vertex != from; vertex = parent[vertex]) {
        const std::size_t tail = parent[vertex];
        const Edge& edge = edges_[tail][parent_edge[vertex]];
        if (!active_clause_ids.contains(edge.clause_id)) {
            return false;
        }
        if (!reduce_triple(clauses[edge.clause_id])) {
            return false;
        }
        // The edge `tail -> vertex` stands for `tail` refuting the negation of
        // `vertex`, so both must be equations the clause says at most one of
        // can hold.
        const Equation head = equation_of(tail);
        const Equation refuted = equation_of(vertex ^ 1);
        const auto holds = [this](const Equation& eqn) {
            return std::ranges::any_of(triple_,
                                       [&eqn](const Equation* row) { return *row == eqn; });
        };
        if (head == refuted || !holds(head) || !holds(refuted)) {
            return false;
        }
    }
    return true;
}
#endif

} // namespace xorcle
