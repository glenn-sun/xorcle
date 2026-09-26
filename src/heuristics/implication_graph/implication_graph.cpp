#include "heuristics/implication_graph/implication_graph.h"

#include <algorithm>
#include <array>
#include <cassert>
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
      lookup_scratch_(repr, num_vars),
      reduced_(repr, num_vars),
      row_scratch_(repr, num_vars),
      triple_{Equation(repr, num_vars), Equation(repr, num_vars), Equation(repr, num_vars)} {
    assert(max_pairs_ != 0 && max_descendants_ != 0 && "a run must be allowed one of each");
}

void ImplicationGraph::set_base_seed(const Seed base_seed) {
    rng_ = make_rng(base_seed, RngComponent::implication_graph_sample);
}

void ImplicationGraph::reconfigure(const std::size_t num_vars) {
    num_vars_ = num_vars;
    false_eqn_ = Equation::eq_false(repr_, num_vars);
    lookup_scratch_.resize(num_vars);
    reduced_ = RowSpace(repr_, num_vars);
    row_scratch_.resize(num_vars);
    for (Equation& row : triple_) {
        row.resize(num_vars);
    }
    spare_rows_.clear();
    key_of_.clear();
    key_eqn_.clear();
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
    while (reduced_.rows() != 0) {
        reduced_.pop_last_row();
    }
    for (const Equation& stored : clause) {
        row_scratch_.assign_from(stored);
        root_span.reduce(row_scratch_);
        reduced_.add_row(row_scratch_);
    }
    // The root trail either narrows the clause or, once `0 = 1` is in the
    // linear negation, satisfies it.
    if (reduced_.rows() != 2 || reduced_.pivot_col_idxs().test(num_vars_)) {
        return false;
    }
    triple_[0].assign_from(reduced_.row(0));
    triple_[1].assign_from(reduced_.row(1));
    triple_[2].assign_from(reduced_.row(0));
    triple_[2].plus_assign(reduced_.row(1));
    return true;
}

std::size_t ImplicationGraph::vertex_of(const Equation& eqn) {
    assert(!eqn.is_eq_true() && !eqn.is_eq_false() &&
           "an implication graph vertex carries no variable");

    if (const auto found = key_of_.find(eqn); found != key_of_.end()) {
        return 2 * found->second;
    }
    lookup_scratch_.assign_from(eqn);
    lookup_scratch_.plus_assign(false_eqn_);
    if (const auto found = key_of_.find(lookup_scratch_); found != key_of_.end()) {
        return 2 * found->second + 1;
    }

    const std::size_t key = key_eqn_.size();
    key_eqn_.push_back(&key_of_.emplace(eqn, key).first->first);
    if (arcs_.size() < 2 * key_eqn_.size()) {
        arcs_.resize(2 * key_eqn_.size());
    }
    descendant_stamp_.resize(2 * key_eqn_.size(), 0);
    other_descendant_stamp_.resize(2 * key_eqn_.size(), 0);
    return 2 * key;
}

Equation ImplicationGraph::equation_of(const std::size_t vertex) const {
    Equation eqn = *key_eqn_[vertex / 2];
    if ((vertex & 1) != 0) {
        eqn.plus_assign(false_eqn_);
    }
    return eqn;
}

void ImplicationGraph::add_clause(const ClauseId clause_id) {
    const std::array<std::size_t, 3> vertices{vertex_of(triple_[0]), vertex_of(triple_[1]),
                                              vertex_of(triple_[2])};
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

void ImplicationGraph::sample_pairs(ImplicationGraphRun& summary) {
    sampled_pairs_.clear();
    const std::size_t num_vertices = 2 * key_eqn_.size();
    for (std::size_t vertex = 0; vertex < num_vertices; vertex += 2) {
        if (!arcs_[vertex].empty() || !arcs_[vertex + 1].empty()) {
            sampled_pairs_.push_back(vertex);
        }
    }
    summary.pairs_eligible = sampled_pairs_.size();
    if (sampled_pairs_.size() <= max_pairs_) {
        return;
    }
    // Partial Fisher-Yates: the first `max_pairs_` places end up holding a
    // uniformly random subset of the eligible pairs.
    for (std::size_t i = 0; i < max_pairs_; ++i) {
        assert(sampled_pairs_.size() - i <= std::numeric_limits<int>::max() &&
               "the eligible pairs outgrew the sampler");
        const std::size_t pick =
            i + static_cast<std::size_t>(
                    random_int(rng_, static_cast<int>(sampled_pairs_.size() - i)));
        std::swap(sampled_pairs_[i], sampled_pairs_[pick]);
    }
    sampled_pairs_.resize(max_pairs_);
    summary.at_the_pair_cap = true;
}

ImplicationGraphRun ImplicationGraph::run(const std::vector<Clause>& clauses,
                                          const std::vector<std::uint8_t>& clause_is_active,
                                          const RowSpace& root_span,
                                          ImplicationGraphProofRecord* proof) {
    assert(clause_is_active.size() == clauses.size() &&
           "the clause active flags are desynchronized");

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

    key_of_.clear();
    key_eqn_.clear();
    for (std::vector<Arc>& arcs : arcs_) {
        arcs.clear();
    }
    num_width_2_clauses_ = 0;

    for (ClauseId clause_id = 0; clause_id < clauses.size(); ++clause_id) {
        if (clause_is_active[clause_id] == 0) {
            continue;
        }
        if (reduce_triple(clauses[clause_id], root_span)) {
            add_clause(clause_id);
        }
    }

    if (!key_eqn_.empty()) {
        walk_span_intersections(root_span, clauses, clause_is_active, summary, proof);
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
    const std::vector<std::uint8_t>& clause_is_active, const RowSpace& root_span) {
    const std::size_t num_vertices = 2 * key_eqn_.size();
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
        if (clause_is_active[arc.clause_id] == 0) {
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
            return std::find(triple_.begin(), triple_.end(), eqn) != triple_.end();
        };
        if (head == refuted || !holds(head) || !holds(refuted)) {
            return false;
        }
    }
    return true;
}
#endif

} // namespace xorcle
