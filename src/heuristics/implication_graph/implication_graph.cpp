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
      false_eqn_(Equation::eq_false(repr, num_vars)),
      rng_(make_rng(default_seed, RngComponent::implication_graph_sample)),
      reduced_(repr, num_vars),
      row_sum_(repr, num_vars) {
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
    false_eqn_ = Equation::eq_false(repr_, num_vars);
    reduced_ = RowSpace(repr_, num_vars);
    row_sum_.resize(num_vars);
    spare_rows_.clear();
    clear_keys();
    key_last_examined_.clear();
    graph_clauses_.clear();
    for (std::vector<Arc>& arcs : arcs_) {
        arcs.clear();
    }
    num_width_2_clauses_ = 0;
}

Equation ImplicationGraph::spare_row(const std::size_t num_vars) {
    if (spare_rows_.empty()) {
        return Equation(repr_, num_vars);
    }
    Equation row = std::move(spare_rows_.back());
    spare_rows_.pop_back();
    if (row.num_vars() != num_vars) {
        row.resize(num_vars);
    }
    return row;
}

void ImplicationGraph::clear_span(RowSpace& span) {
    while (span.rows() != 0) {
        spare_rows_.push_back(span.pop_row(span.rows() - 1));
    }
}

bool ImplicationGraph::reduce_triple(const Clause& clause, const RowSpace& root_span) {
    if (clause.rows() != 2) {
        return false;
    }
    // An empty root trail reduces nothing, and a clause's rows are already as
    // `reduced_` would hold them.
    const RowSpace* rows = &clause;
    if (root_span.rows() != 0) {
        reduce_into_reduced(clause, root_span);
        rows = &reduced_;
    }
#ifndef NDEBUG
    if (rows == &clause) {
        reduce_into_reduced(clause, root_span);
        assert(reduced_.rows() == 2 && reduced_.row(0) == clause.row(0) &&
               reduced_.row(1) == clause.row(1) &&
               "a clause's rows are not as a row space of them holds them");
    }
#endif
    // The root trail either narrows the clause or, once `0 = 1` is in the
    // linear negation, satisfies it.
    if (rows->rows() != 2 || rows->pivot_col_idxs().test(num_vars_)) {
        return false;
    }
    row_sum_.assign_from(rows->row(0));
    row_sum_.plus_assign(rows->row(1));
    triple_ = {&rows->row(0), &rows->row(1), &row_sum_};
    return true;
}

void ImplicationGraph::reduce_into_reduced(const Clause& clause, const RowSpace& root_span) {
    clear_span(reduced_);
    for (const Equation& stored : clause) {
        Equation row = spare_row(num_vars_);
        row.assign_from(stored);
        root_span.reduce(row);
        reduced_.add_row(std::move(row));
    }
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
    if (arcs_.size() < 2 * num_keys_) {
        arcs_.resize(2 * num_keys_);
    }
    descendant_stamp_.resize(2 * num_keys_, 0);
    other_descendant_stamp_.resize(2 * num_keys_, 0);
    key_last_examined_.push_back(0);
    return 2 * key;
}

Equation ImplicationGraph::equation_of(const std::size_t vertex) const {
    Equation eqn = key_eqn_[vertex / 2];
    if ((vertex & 1) != 0) {
        eqn.plus_assign(false_eqn_);
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
                arcs_[from].push_back(Arc{to ^ 1, clause_id});
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
            for (const Arc& arc : arcs_[reached[head]]) {
                ++edges_visited;
                if (stamp[arc.target] != mark) {
                    stamp[arc.target] = mark;
                    reached.push_back(arc.target);
                    if (parent_clause != nullptr) {
                        parent_clause->push_back(arc.clause_id);
                    }
                }
            }
        }
        if (reached.size() > max_descendants_) {
            // Any subset of the descendants spans a subspace of the full span,
            // so a unit found in the intersection still holds and a refuted side
            // is still refuted.
            for (std::size_t i = next_begin; i < max_descendants_; ++i) {
                assert(reached.size() - i <= std::numeric_limits<int>::max() &&
                       "a distance level outgrew the sampler");
                const std::size_t pick =
                    i + static_cast<std::size_t>(
                            random_int(rng_, static_cast<int>(reached.size() - i)));
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

    // An arc `u -> w` has the mirror `not w -> not u`, so what reaches a vertex
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
            for (const Arc& arc : arcs_[search_queue_[head]]) {
                if (latest_first_read_[arc.target] == 0) {
                    latest_first_read_[arc.target] = read;
                    search_queue_.push_back(arc.target);
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
        if (!arcs_[vertex].empty() || !arcs_[vertex + 1].empty()) {
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
        assert(sampled_pairs_.size() - i <= std::numeric_limits<int>::max() &&
               "the dirty pairs outgrew the sampler");
        const std::size_t pick =
            i + static_cast<std::size_t>(
                    random_int(rng_, static_cast<int>(sampled_pairs_.size() - i)));
        std::swap(sampled_pairs_[i], sampled_pairs_[pick]);
    }
    sampled_pairs_.resize(max_pairs_);
    summary.at_the_pair_cap = true;
}

ImplicationGraphRun ImplicationGraph::run(const std::vector<Clause>& clauses,
                                          ActiveClauseIds active_clause_ids,
                                          const RowSpace& root_span,
                                          ImplicationGraphProofRecord* proof) {
    ImplicationGraphRun summary;
    if (proof != nullptr) {
        proof->clear();
    }
    // A refuted root implies every equation and its negation alike, so reducing
    // by it would strip the constant bits that tell the two apart.
    if (root_span.pivot_col_idxs().test(num_vars_)) {
        return summary;
    }
    summary.did_work = true;

    clear_keys();
    for (std::vector<Arc>& arcs : arcs_) {
        arcs.clear();
    }
    num_width_2_clauses_ = 0;
    graph_clauses_.clear();
    key_last_examined_.clear();

    // A root trail with rows changes what a clause reduces to without
    // rewriting it, so nothing read or examined before or during this run counts.
    if (root_span.rows() != 0) {
        clause_runs_.clear();
    }
    clause_runs_.resize(clauses.size());
    ++num_runs_;

    for (const ClauseId clause_id : active_clause_ids) {
        if (reduce_triple(clauses[clause_id], root_span)) {
            add_clause(clause_id);
        }
    }

    if (num_keys_ != 0) {
        assert(all_arcs_are_clause_backed(clauses, active_clause_ids, root_span) &&
               "an arc of the implication graph is not witnessed by its clause");
        const auto start = std::chrono::steady_clock::now();
        mark_dirty_pairs();
        sample_pairs(summary);
        walk_span_intersections(root_span, clauses, active_clause_ids, summary, proof);
        stamp_examined_pairs();
        const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start;
        summary.span_intersection_time_s += elapsed.count();
        assert(full_examination_yields(summary, root_span, clauses, active_clause_ids) &&
               "a clean pair held a unit");
    }
    if (root_span.rows() != 0) {
        clause_runs_.clear();
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
    ActiveClauseIds active_clause_ids, const RowSpace& root_span) {
    const std::size_t num_vertices = 2 * num_keys_;
    std::vector<std::size_t> parent(num_vertices, no_index);
    std::vector<std::size_t> parent_arc(num_vertices, no_index);
    std::vector<std::uint8_t> seen(num_vertices, 0);
    std::vector<std::size_t> queue{from};
    seen[from] = 1;
    for (std::size_t head = 0; head < queue.size() && seen[to] == 0; ++head) {
        const std::size_t vertex = queue[head];
        for (std::size_t i = 0; i < arcs_[vertex].size(); ++i) {
            const std::size_t target = arcs_[vertex][i].target;
            if (seen[target] == 0) {
                seen[target] = 1;
                parent[target] = vertex;
                parent_arc[target] = i;
                queue.push_back(target);
            }
        }
    }
    if (seen[to] == 0) {
        return false;
    }

    for (std::size_t vertex = to; vertex != from; vertex = parent[vertex]) {
        const std::size_t tail = parent[vertex];
        const Arc& arc = arcs_[tail][parent_arc[vertex]];
        if (!active_clause_ids.contains(arc.clause_id)) {
            return false;
        }
        if (!reduce_triple(clauses[arc.clause_id], root_span)) {
            return false;
        }
        // The arc `tail -> vertex` stands for `tail` refuting the negation of
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
