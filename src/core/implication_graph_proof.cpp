/// @file implication_graph_proof.cpp
/// @brief The lines proving the units the implication graph feeds.
///
/// Each unit is a width-1 clause, and the checker takes a line only if unit
/// propagation from the line's own rows falsifies a clause, so a line replays
/// the edges the run followed, one width-2 clause at a time. See
/// arXiv:2605.15002 for LRUP(⊕).

#include <cassert>
#include <cstddef>
#include <span>
#include <vector>

#include "algebra/equation.h"
#include "algebra/row_echelon_matrix.h"
#include "core/clause_id.h"
#include "core/hint_trail.h"
#include "core/solver.h"
#include "heuristics/implication_graph/implication_graph.h"
#include "io/proof_writer.h"

namespace xorcle {

std::vector<Equation> Solver::lift_clause(const ClauseId id) const {
    const Clause& clause = clause_database_.clause(id);
    std::vector<Equation> rows;
    rows.reserve(clause.rows());
    for (const Equation& row : clause) {
        rows.push_back(Equation::from_bits(
            repr_, original_num_vars_,
            lift_row(row, active_var_to_original_, original_num_vars_)));
    }
    return rows;
}

Solver::Citations Solver::replay_edges(std::span<const ClauseId> edges, HintTrail& trail) {
    Citations cited;
    for (const ClauseId edge : edges) {
        const std::vector<Equation> rows = lift_clause(edge);
        const Hint hint = trail.cite(rows);
        assert(hint != Hint::not_unit && "an edge's clause does not propagate on the replay");
        if (hint == Hint::known) {
            continue;
        }
        cited.hints.push_back(proof_id_of_clause_[edge]);
        if (hint == Hint::conflicting) {
            cited.conflicted = true;
            return cited;
        }
        if (trail.is_refuted()) {
            // The trail spans `0 = 1` now, so it holds the negation of what it
            // holds, and the clause just cited has its whole linear negation on
            // it.
            [[maybe_unused]] const Hint again = trail.cite(rows);
            assert(again == Hint::conflicting &&
                   "a refuted trail does not falsify the clause that refuted it");
            cited.hints.push_back(proof_id_of_clause_[edge]);
            cited.conflicted = true;
            return cited;
        }
    }
    return cited;
}

std::vector<ClauseId> Solver::write_implication_graph_lines(
    const ImplicationGraphProofRecord& record) {
    using Kind = ImplicationGraphProofStep::Kind;
    assert(proof_writer_ != nullptr && "no proof to record the implication graph's units in");

    HintTrail trail(repr_, original_num_vars_);
    std::vector<ClauseId> step_line(record.steps.size(), 0);
    std::vector<Equation> rows;
    std::vector<ClauseId> hints;

    const auto lifted = [&](const Equation& eqn) {
        return Equation::from_bits(repr_, original_num_vars_,
                                   lift_row(eqn, active_var_to_original_, original_num_vars_));
    };
    const auto write = [&] {
        std::vector<ProofRow> proof_rows;
        proof_rows.reserve(rows.size());
        for (const Equation& row : rows) {
            proof_rows.push_back(row.bits());
        }
        const ClauseId id = next_proof_clause_id_++;
        proof_writer_->write_learned(id, proof_rows, hints);
        return id;
    };
    // Every step's line proves the width-1 clause of its equation, so citing it
    // is citing that one row.
    const auto cite_step = [&](const std::size_t step_idx) {
        Equation row = lifted(record.steps[step_idx].equation);
        row.negate();
        const Hint hint = trail.cite(std::span<const Equation>(&row, 1));
        assert(hint != Hint::known && hint != Hint::not_unit &&
               "an earlier step's unit is no use to the line citing it");
        hints.push_back(step_line[step_idx]);
        return hint == Hint::conflicting;
    };
    const auto replay = [&](std::span<const ClauseId> edges) {
        const Citations cited = replay_edges(edges, trail);
        hints.insert(hints.end(), cited.hints.begin(), cited.hints.end());
        return cited.conflicted;
    };

    for (std::size_t step_idx = 0; step_idx < record.steps.size(); ++step_idx) {
        const ImplicationGraphProofStep& step = record.steps[step_idx];
        const std::span<const ClauseId> edges(record.edges.data() + step.edges_begin,
                                             step.edges_end - step.edges_begin);
        Equation negated = lifted(step.equation);
        negated.negate();

        rows.clear();
        hints.clear();
        [[maybe_unused]] bool conflicted = false;
        switch (step.kind) {
            case Kind::refuted:
                // Assume what the run refuted, and follow it to `0 = 1`.
                rows.push_back(negated);
                trail.begin_line(rows);
                conflicted = replay(edges);
                break;
            case Kind::reaches_refuted:
                rows.push_back(negated);
                trail.begin_line(rows);
                conflicted = replay(edges) || cite_step(step.source);
                break;
            case Kind::one_side:
                rows.push_back(negated);
                trail.begin_line(rows);
                conflicted = cite_step(step.source) || replay(edges);
                break;
            case Kind::both_sides: {
                // Neither side is refuted, so the two halves of the case split
                // are clauses of their own, each cited once.
                const Equation assumed = lifted(step.assumed);
                Equation negated_assumed = assumed;
                negated_assumed.negate();
                const std::span<const ClauseId> other_edges(
                    record.edges.data() + step.other_edges_begin,
                    step.other_edges_end - step.other_edges_begin);

                const std::vector<Equation> first_rows{assumed, negated};
                const std::vector<Equation> second_rows{negated_assumed, negated};

                rows = first_rows;
                trail.begin_line(rows);
                [[maybe_unused]] const bool first = replay(edges);
                assert(first && "one half of a two-sided unit reaches no conflict");
                const ClauseId first_line = write();

                rows = second_rows;
                hints.clear();
                trail.begin_line(rows);
                [[maybe_unused]] const bool second = replay(other_edges);
                assert(second && "the other half of a two-sided unit reaches no conflict");
                const ClauseId second_line = write();

                // Without the equation, one half leaves the negation of what the
                // other half then falsifies.
                rows = {negated};
                hints = {first_line, second_line};
                trail.begin_line(rows);
                conflicted = trail.cite(first_rows) == Hint::propagated &&
                             trail.cite(second_rows) == Hint::conflicting;
                break;
            }
        }
        assert(conflicted && "a unit the implication graph found has no replay that conflicts");
        step_line[step_idx] = write();
    }
    std::vector<ClauseId> unit_lines;
    unit_lines.reserve(record.unit_idx_to_step_idx.size());
    for (const std::size_t step_idx : record.unit_idx_to_step_idx) {
        unit_lines.push_back(step_line[step_idx]);
    }
    return unit_lines;
}

} // namespace xorcle
