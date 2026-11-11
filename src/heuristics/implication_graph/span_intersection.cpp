/// @file span_intersection.cpp
/// @brief The rule that reads the implication graph: the intersection of the
/// spans of the two sides of a vertex pair.

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <vector>

#include "algebra/dense/dense_gf2_vector.h"
#include "algebra/equation.h"
#include "algebra/reduce_result.h"
#include "algebra/row_echelon_matrix.h"
#include "heuristics/implication_graph/implication_graph.h"

namespace xorcle {

namespace {

#ifndef NDEBUG
/// @brief Vertices below which every descendant is also checked path by path,
/// which costs a search of the whole graph per descendant.
constexpr std::size_t path_check_vertices = 500;
#endif

} // namespace

void ImplicationGraph::walk_span_intersections(
    [[maybe_unused]] const std::vector<Clause>& clauses,
    [[maybe_unused]] ActiveClauseIds active_clause_ids,
    ImplicationGraphRun& summary, ImplicationGraphProofRecord* const proof) {
    using Kind = ImplicationGraphProofStep::Kind;
    const std::size_t num_vertices = 2 * num_keys_;

    RowSpace accumulated(repr_, num_vars_);
    Equation candidate(repr_, num_vars_);

    // Where the vertices have fewer set bits than words, a pair's spans are
    // over only the columns its descendants use, kept in order, which leaves
    // every echelon form and intersection as it is over all of them.
    std::size_t num_key_bits = 0;
    for (std::size_t key = 0; key < num_keys_; ++key) {
        num_key_bits += key_eqn_[key].num_set_bits();
    }
    const bool local_columns =
        num_key_bits < num_keys_ * DenseGF2Vector::word_count(num_vars_ + 1);
    // The set bits of each key a descendant search has reached, listed when
    // it first does.
    std::vector<std::size_t> key_bits;
    std::vector<std::size_t> key_bits_begin(local_columns ? num_keys_ : 0, no_index);
    std::vector<std::size_t> key_bits_end(local_columns ? num_keys_ : 0);
    const auto list_key_bits = [&](const std::vector<std::size_t>& reached) {
        for (const std::size_t vertex : reached) {
            const std::size_t key = vertex / 2;
            if (key_bits_begin[key] == no_index) {
                key_bits_begin[key] = key_bits.size();
                const std::vector<std::size_t> key_eqn_bits = key_eqn_[key].bits();
                key_bits.insert(key_bits.end(), key_eqn_bits.begin(), key_eqn_bits.end());
                key_bits_end[key] = key_bits.size();
            }
        }
    };
    std::vector<std::size_t> col_idx_to_local_col_idx(local_columns ? num_vars_ + 1 : 0, no_index);
    std::vector<std::size_t> local_col_idx_to_col_idx;
    std::vector<std::size_t> bits;
    std::size_t num_span_vars = num_vars_;
    spare_rows_.resize(num_vars_ + 1);
    // The two sides of the pair the walk is on, and their sum, whose rank gives
    // the dimension of their intersection.
    RowSpace side_a(repr_, num_span_vars);
    RowSpace side_b(repr_, num_span_vars);
    RowSpace both_sides(repr_, num_span_vars);
    const auto use_columns_of = [&](const std::vector<std::size_t>& first,
                                    const std::vector<std::size_t>* const second) {
        for (RowSpace* const span : {&side_a, &side_b, &both_sides}) {
            spare_rows_.give_rows_of(*span);
        }
        if (!local_columns) {
            return;
        }
        for (const std::size_t col_idx : local_col_idx_to_col_idx) {
            col_idx_to_local_col_idx[col_idx] = no_index;
        }
        local_col_idx_to_col_idx.clear();
        const auto collect = [&](const std::vector<std::size_t>& reached) {
            list_key_bits(reached);
            for (const std::size_t vertex : reached) {
                for (std::size_t i = key_bits_begin[vertex / 2]; i < key_bits_end[vertex / 2]; ++i) {
                    const std::size_t col_idx = key_bits[i];
                    if (col_idx != num_vars_ && col_idx_to_local_col_idx[col_idx] == no_index) {
                        col_idx_to_local_col_idx[col_idx] = 0;
                        local_col_idx_to_col_idx.push_back(col_idx);
                    }
                }
            }
        };
        collect(first);
        if (second != nullptr) {
            collect(*second);
        }
        std::sort(local_col_idx_to_col_idx.begin(), local_col_idx_to_col_idx.end());
        for (std::size_t local_col_idx = 0; local_col_idx < local_col_idx_to_col_idx.size();
             ++local_col_idx) {
            col_idx_to_local_col_idx[local_col_idx_to_col_idx[local_col_idx]] = local_col_idx;
        }
        num_span_vars = local_col_idx_to_col_idx.size();
        col_idx_to_local_col_idx[num_vars_] = num_span_vars;
        spare_rows_.resize(num_span_vars + 1);
        side_a = RowSpace(repr_, num_span_vars);
        side_b = RowSpace(repr_, num_span_vars);
        both_sides = RowSpace(repr_, num_span_vars);
    };
    const auto to_global = [&](const Equation& row) {
        if (!local_columns) {
            return row;
        }
        bits = row.bits();
        for (std::size_t& bit : bits) {
            bit = bit == num_span_vars ? num_vars_ : local_col_idx_to_col_idx[bit];
        }
        Equation eqn(repr_, num_vars_);
        eqn.assign_bits(bits);
        return eqn;
    };
    const auto side_span = [&](RowSpace& span,
                               const std::vector<std::size_t>& reached) -> RowSpace& {
        for (const std::size_t vertex : reached) {
            Equation row = spare_rows_.take();
            if (local_columns) {
                bits.clear();
                for (std::size_t i = key_bits_begin[vertex / 2]; i < key_bits_end[vertex / 2];
                     ++i) {
                    bits.push_back(col_idx_to_local_col_idx[key_bits[i]]);
                }
                if ((vertex & 1) != 0) {
                    if (!bits.empty() && bits.back() == num_span_vars) {
                        bits.pop_back();
                    } else {
                        bits.push_back(num_span_vars);
                    }
                }
                row.assign_bits(bits);
            } else {
                row.assign_from(key_eqn_[vertex / 2]);
                if ((vertex & 1) != 0) {
                    row.negate();
                }
            }
            // `partial_reduce` is exactly what `add_row` would do before
            // storing, so doing it here leaves the same row.
            if (span.partial_reduce(row).kind != ReduceResult::Kind::eq_true) {
                span.add_row(std::move(row));
            } else {
                spare_rows_.give(std::move(row));
            }
        }
        return span;
    };
    const auto is_refuted = [](const RowSpace& span) {
        return span.pivot_col_idxs().test(span.cols() - 1);
    };

    // A vertex whose successor is refuted is refuted too, so a side reached
    // that way needs no span of its own. Marks are filled in the order the
    // pairs come, so this catches whatever the order has already settled.
    std::vector<std::uint8_t> refuted_mark(num_vertices, 0);
    const auto inherited_refutation = [&](const std::size_t v) -> const Arc* {
        for (const Arc& arc : arcs_[v]) {
            if (refuted_mark[arc.target] != 0) {
                return &arc;
            }
        }
        return nullptr;
    };

    std::vector<ClauseId>* const parents = proof != nullptr ? &parent_clauses_ : nullptr;
    std::vector<ClauseId>* const other_parents =
        proof != nullptr ? &other_parent_clauses_ : nullptr;
    // The step proving each refuted vertex's negation, which a unit drawn from
    // the other side and any vertex reaching this one then cite.
    std::vector<std::size_t> refuted_step;
    if (proof != nullptr) {
        refuted_step.assign(num_vertices, no_proof_step);
    }
    const auto record_refutation = [&](const std::size_t v, const Arc* const inherited,
                                       const std::vector<ClauseId>& replay) {
        refuted_mark[v] = 1;
        if (proof == nullptr) {
            return;
        }
        ImplicationGraphProofStep step;
        step.equation = equation_of(v ^ 1);
        step.arcs_begin = proof->arcs.size();
        if (inherited != nullptr) {
            step.kind = Kind::reaches_refuted;
            step.source = refuted_step[inherited->target];
            assert(step.source != no_proof_step && "a refuted vertex went without a step");
            proof->arcs.push_back(inherited->clause_id);
        } else {
            step.kind = Kind::refuted;
            proof->arcs.insert(proof->arcs.end(), replay.begin(), replay.end());
        }
        step.arcs_end = proof->arcs.size();
        refuted_step[v] = proof->steps.size();
        proof->steps.push_back(std::move(step));
    };

    for (const std::size_t vertex : sampled_pairs_) {
        ++summary.pairs_examined;

        const bool cut_here = mark_descendants(vertex, descendant_stamp_, ++next_mark_,
                                               descendants_, summary.edges_visited, parents);
        const bool cut_there =
            mark_descendants(vertex + 1, other_descendant_stamp_, ++next_mark_,
                             other_descendants_, summary.edges_visited, other_parents);
        summary.truncated_sides += (cut_here ? 1U : 0U) + (cut_there ? 1U : 0U);
#ifndef NDEBUG
        if (num_vertices <= path_check_vertices) {
            for (const std::size_t reached : descendants_) {
                assert(implication_is_clause_backed(vertex, reached, clauses, active_clause_ids) &&
                       "a descendant is not reached through clause-backed arcs");
            }
            for (const std::size_t reached : other_descendants_) {
                assert(implication_is_clause_backed(vertex + 1, reached, clauses,
                                                    active_clause_ids) &&
                       "a descendant is not reached through clause-backed arcs");
            }
        }
#endif
        // A side with no out-arc spans its own equation alone, which is never
        // `0 = 1`, so the intersection is that equation exactly when the other
        // side is refuted and is `0 = 0` otherwise.
        const bool lone_first = arcs_[vertex].empty();
        const bool one_sided = lone_first || arcs_[vertex + 1].empty();
        if (one_sided) {
            const std::size_t deep_vertex = lone_first ? vertex + 1 : vertex;
            const Arc* const inherited = inherited_refutation(deep_vertex);
            bool deep_refuted = inherited != nullptr;
            if (!deep_refuted) {
                const std::vector<std::size_t>& deep = lone_first ? other_descendants_ : descendants_;
                use_columns_of(deep, nullptr);
                deep_refuted = is_refuted(side_span(side_a, deep));
            }
            summary.refuted_sides += deep_refuted ? 1 : 0;
            if (deep_refuted) {
                record_refutation(deep_vertex, inherited,
                                  lone_first ? other_parent_clauses_ : parent_clauses_);
                Equation unit = equation_of(lone_first ? vertex : vertex + 1);
                candidate.assign_from(unit);
                if (accumulated.partial_reduce(candidate).kind != ReduceResult::Kind::eq_true) {
                    accumulated.add_row(candidate);
                    if (proof != nullptr) {
                        proof->unit_idx_to_step_idx.push_back(refuted_step[deep_vertex]);
                    }
                    summary.units.push_back(std::move(unit));
                }
            }
            continue;
        }

        const Arc* const here_inherited = inherited_refutation(vertex);
        const Arc* const there_inherited = inherited_refutation(vertex + 1);
        bool here_refuted = here_inherited != nullptr;
        bool there_refuted = there_inherited != nullptr;
        use_columns_of(descendants_, &other_descendants_);
        const RowSpace& here = here_refuted ? side_a : side_span(side_a, descendants_);
        if (!here_refuted) {
            here_refuted = is_refuted(here);
        }
        const RowSpace& there = there_refuted ? side_b : side_span(side_b, other_descendants_);
        if (!there_refuted) {
            there_refuted = is_refuted(there);
        }
        summary.refuted_sides += (here_refuted ? 1U : 0U) + (there_refuted ? 1U : 0U);
        if (here_refuted) {
            record_refutation(vertex, here_inherited, parent_clauses_);
        }
        if (there_refuted) {
            record_refutation(vertex + 1, there_inherited, other_parent_clauses_);
        }

        std::vector<Equation> intersection;
        // The step already proving each equation of the intersection, where the
        // pair's two refutations are what proves them.
        std::vector<std::size_t> intersection_step;
        Kind kind = Kind::both_sides;
        std::size_t source = no_proof_step;
        if (here_refuted && there_refuted) {
            // Every equation holds, so the root is refuted; the two sides say so
            // in a form the solver can add, which `0 = 1` is not.
            intersection.push_back(equation_of(vertex));
            intersection.push_back(equation_of(vertex + 1));
            if (proof != nullptr) {
                intersection_step = {refuted_step[vertex + 1], refuted_step[vertex]};
            }
        } else if (here_refuted || there_refuted) {
            const RowSpace& other = here_refuted ? there : here;
            for (const Equation& row : other) {
                intersection.push_back(to_global(row));
            }
            kind = Kind::one_side;
            if (proof != nullptr) {
                source = here_refuted ? refuted_step[vertex] : refuted_step[vertex + 1];
            }
        } else {
            // dim(U n W) = dim U + dim W - dim(U + W), over the columns of one
            // side rather than of both laid side by side, so the basis is built
            // only for the few pairs whose intersection is more than `0 = 0`.
            for (const Equation& row : here) {
                both_sides.add_row(spare_rows_.copy_of(row));
            }
            for (const Equation& row : there) {
                Equation copy = spare_rows_.copy_of(row);
                if (both_sides.partial_reduce(copy).kind != ReduceResult::Kind::eq_true) {
                    both_sides.add_row(std::move(copy));
                } else {
                    spare_rows_.give(std::move(copy));
                }
            }
            const std::size_t dim = here.rows() + there.rows() - both_sides.rows();
            if (dim != 0) {
                for (Equation& row : span_intersection(here, there, spare_rows_)) {
                    intersection.push_back(to_global(row));
                    spare_rows_.give(std::move(row));
                }
            }
            assert(intersection.size() == dim && "the span intersection has the wrong dimension");
        }

        // The pair's replays, appended to the pool the first time a unit cites them.
        bool replays_are_pooled = false;
        std::size_t arcs_begin = 0;
        std::size_t arcs_end = 0;
        std::size_t other_arcs_begin = 0;
        std::size_t other_arcs_end = 0;

        for (std::size_t i = 0; i < intersection.size(); ++i) {
            const Equation& eqn = intersection[i];
            if (eqn.is_eq_false()) {
                continue;
            }
            candidate.assign_from(eqn);
            if (accumulated.partial_reduce(candidate).kind == ReduceResult::Kind::eq_true) {
                continue;
            }
            // The span is what matters, so the reduced row is stored while the
            // unit the solver is handed stays as the rule found it.
            accumulated.add_row(candidate);
            // A refuted side stands for the whole space, so it holds every
            // equation and there is nothing to check against its rows.
#ifndef NDEBUG
            const auto holds = [&](const RowSpace& span) {
                Equation reduced(repr_, num_span_vars);
                if (local_columns) {
                    std::vector<std::size_t> local_bits = eqn.bits();
                    for (std::size_t& bit : local_bits) {
                        bit = col_idx_to_local_col_idx[bit];
                    }
                    reduced.assign_bits(local_bits);
                } else {
                    reduced.assign_from(eqn);
                }
                span.reduce(reduced);
                return reduced.is_eq_true();
            };
            assert((here_refuted || holds(here)) && (there_refuted || holds(there)) &&
                   "a span-intersection unit is not in both sides' spans");
#endif
            if (proof != nullptr) {
                if (!intersection_step.empty()) {
                    proof->unit_idx_to_step_idx.push_back(intersection_step[i]);
                } else {
                    if (!replays_are_pooled) {
                        replays_are_pooled = true;
                        const std::vector<ClauseId>& replay =
                            kind == Kind::one_side && here_refuted ? other_parent_clauses_
                                                                  : parent_clauses_;
                        arcs_begin = proof->arcs.size();
                        proof->arcs.insert(proof->arcs.end(), replay.begin(), replay.end());
                        arcs_end = proof->arcs.size();
                        if (kind == Kind::both_sides) {
                            other_arcs_begin = proof->arcs.size();
                            proof->arcs.insert(proof->arcs.end(), other_parent_clauses_.begin(),
                                               other_parent_clauses_.end());
                            other_arcs_end = proof->arcs.size();
                        }
                    }
                    ImplicationGraphProofStep step;
                    step.kind = kind;
                    step.equation = eqn;
                    if (kind == Kind::both_sides) {
                        step.assumed = equation_of(vertex);
                    }
                    step.source = source;
                    step.arcs_begin = arcs_begin;
                    step.arcs_end = arcs_end;
                    step.other_arcs_begin = other_arcs_begin;
                    step.other_arcs_end = other_arcs_end;
                    proof->unit_idx_to_step_idx.push_back(proof->steps.size());
                    proof->steps.push_back(std::move(step));
                }
            }
            summary.units.push_back(eqn);
        }
    }
}

#ifndef NDEBUG
bool ImplicationGraph::full_examination_yields(const ImplicationGraphRun& run,
                                               const std::vector<Clause>& clauses,
                                               ActiveClauseIds active_clause_ids) {
    const Rng rng = rng_;
    sampled_pairs_.clear();
    for (std::size_t vertex = 0; vertex < 2 * num_keys_; vertex += 2) {
        sampled_pairs_.push_back(vertex);
    }
    ImplicationGraphRun full;
    walk_span_intersections(clauses, active_clause_ids, full, nullptr);
    rng_ = rng;
    a_side_was_truncated_ = a_side_was_truncated_ || full.truncated_sides != 0;
    if (a_side_was_truncated_ || run.at_the_pair_cap || full.units == run.units) {
        return true;
    }
    // A unit of an earlier run that propagation has yet to reach is still a
    // width-1 clause, and the clean pair it came from yields it again.
    RowSpace known(repr_, num_vars_);
    for (const ClauseId clause_id : active_clause_ids) {
        if (clauses[clause_id].rows() == 1) {
            Equation unit = clauses[clause_id].row(0);
            unit.negate();
            known.add_row(std::move(unit));
        }
    }
    const std::size_t num_known = known.rows();
    for (const Equation& unit : run.units) {
        known.add_row(unit);
    }
    return num_known != 0 &&
           std::ranges::all_of(full.units, [&known](Equation unit) {
               known.reduce(unit);
               return unit.is_eq_true();
           });
}

bool ImplicationGraph::all_arcs_are_clause_backed(
    const std::vector<Clause>& clauses, ActiveClauseIds active_clause_ids) {
    const auto holds = [this](const Equation& eqn) {
        return std::ranges::any_of(triple_, [&eqn](const Equation* row) { return *row == eqn; });
    };
    for (std::size_t from = 0; from < 2 * num_keys_; ++from) {
        for (const Arc& arc : arcs_[from]) {
            if (!active_clause_ids.contains(arc.clause_id) ||
                !reduce_triple(clauses[arc.clause_id])) {
                return false;
            }
            const Equation head = equation_of(from);
            const Equation refuted = equation_of(arc.target ^ 1);
            if (head == refuted || !holds(head) || !holds(refuted)) {
                return false;
            }
        }
    }
    return true;
}
#endif

} // namespace xorcle
