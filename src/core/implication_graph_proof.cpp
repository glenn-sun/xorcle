/// @file implication_graph_proof.cpp
/// @brief The lines proving the units the implication graph feeds.
///
/// Each unit is a width-1 clause, and the checker takes a line only if unit
/// propagation from the line's own rows falsifies a clause, so a line replays
/// the arcs the run followed, one width-2 clause at a time. See
/// arXiv:2605.15002 for LRUP(⊕).

#include <cassert>
#include <cstddef>
#include <span>
#include <vector>

#include "algebra/equation.h"
#include "algebra/row_echelon_matrix.h"
#include "core/clause_id.h"
#include "core/solver.h"
#include "heuristics/implication_graph/implication_graph.h"
#include "io/proof_raw.h"
#include "io/proof_raw_file.h"
#include "util/index_set.h"

namespace xorcle {

namespace {

/// @brief What citing one clause does to the checker's trail.
enum class Hint {
    conflicting, ///< Every row of the clause is on the trail.
    propagated,  ///< One row is left, and its negation is now on the trail.
    known,       ///< That negation was already there, which the checker refuses.
    not_unit,    ///< Two rows are left, which the checker refuses.
};

} // namespace

class HintTrail {
public:
    HintTrail(const EquationRepr repr, const std::size_t num_vars)
        : span_(repr, num_vars),
          false_eqn_(Equation::eq_false(repr, num_vars)),
          residual_(repr, num_vars),
          scratch_(repr, num_vars) {}

    /// @brief Begin a line, whose own rows the checker assumes.
    void restart(std::span<const Equation> rows) {
        while (span_.rows() != 0) {
            span_.pop_last_row();
        }
        for (const Equation& row : rows) {
            span_.add_row(row);
        }
    }

    bool holds(const Equation& eqn) {
        scratch_.assign_from(eqn);
        span_.reduce(scratch_);
        return scratch_.is_eq_true();
    }

    bool is_refuted() { return holds(false_eqn_); }

    /// @brief Cite the clause whose linear negation is @p rows.
    Hint cite(std::span<const Equation> rows) {
        bool any = false;
        for (const Equation& row : rows) {
            scratch_.assign_from(row);
            span_.reduce(scratch_);
            if (scratch_.is_eq_true()) {
                continue;
            }
            if (!any) {
                residual_.assign_from(scratch_);
                any = true;
            } else if (!(residual_ == scratch_)) {
                return Hint::not_unit;
            }
        }
        if (!any) {
            return Hint::conflicting;
        }
        residual_.plus_assign(false_eqn_);
        if (holds(residual_)) {
            return Hint::known;
        }
        span_.add_row(residual_);
        return Hint::propagated;
    }

private:
    RowSpace span_;
    Equation false_eqn_;
    Equation residual_;
    Equation scratch_;
};

std::vector<Equation> Solver::lift_clause(const ClauseId id) const {
    const Clause& clause = selector_->clause(id);
    std::vector<Equation> rows;
    rows.reserve(clause.rows());
    for (const Equation& row : clause) {
        rows.push_back(Equation::from_bits(
            repr_, original_num_vars_,
            lift_row(row, active_var_to_original_, original_num_vars_)));
    }
    return rows;
}

Solver::Citations Solver::cite_root_units(std::span<const Equation> rows, HintTrail& trail) {
    const Equation original_false = Equation::eq_false(repr_, original_num_vars_);
    Equation probe(repr_, original_num_vars_);
    Equation negated(repr_, original_num_vars_);
    Citations cited;
    for (const Equation& row : rows) {
        probe.assign_from(row);
        proof_root_span_.reduce_get_coefficients(probe).for_each_set([&](const std::size_t
                                                                            row_idx) {
            if (cited.conflicted) {
                return;
            }
            const Equation& unit = proof_root_span_.row(row_idx);
            if (trail.holds(unit)) {
                return;
            }
            negated.assign_from(unit);
            negated.plus_assign(original_false);
            const Hint hint = trail.cite(std::span<const Equation>(&negated, 1));
            assert((hint == Hint::propagated || hint == Hint::conflicting) &&
                   "a root unit the trail does not hold neither propagates nor conflicts");
            cited.hints.push_back(proof_col_idx_to_unit_id_[*unit.leading_bit()]);
            cited.conflicted = hint == Hint::conflicting;
        });
        if (cited.conflicted) {
            return cited;
        }
    }
    return cited;
}

Solver::Citations Solver::replay_arcs(std::span<const ClauseId> arcs, HintTrail& trail) {
    Citations cited;
    for (const ClauseId arc : arcs) {
        const std::vector<Equation> rows = lift_clause(arc);
        const Citations units = cite_root_units(rows, trail);
        cited.hints.insert(cited.hints.end(), units.hints.begin(), units.hints.end());
        if (units.conflicted) {
            cited.conflicted = true;
            return cited;
        }
        const Hint hint = trail.cite(rows);
        assert(hint != Hint::not_unit && "an arc's clause does not propagate on the replay");
        if (hint == Hint::known) {
            continue;
        }
        cited.hints.push_back(proof_id_of_clause_[arc]);
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
            cited.hints.push_back(proof_id_of_clause_[arc]);
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

    const Equation original_false = Equation::eq_false(repr_, original_num_vars_);
    HintTrail trail(repr_, original_num_vars_);
    std::vector<ClauseId> step_line(record.steps.size(), 0);
    std::vector<ClauseId> unit_lines;
    std::vector<Equation> rows;
    std::vector<ClauseId> hints;

    const auto lifted = [&](const Equation& eqn) {
        return Equation::from_bits(repr_, original_num_vars_,
                                   lift_row(eqn, active_var_to_original_, original_num_vars_));
    };
    const auto write = [&] {
        std::vector<RawRow> raw_rows;
        raw_rows.reserve(rows.size());
        for (const Equation& row : rows) {
            raw_rows.push_back(row.bits());
        }
        const ClauseId id = next_proof_clause_id_++;
        proof_writer_->write_learned(id, raw_rows, hints);
        return id;
    };
    // Every step's line proves the width-1 clause of its equation, so citing it
    // is citing that one row.
    const auto cite_step = [&](const std::size_t index) {
        Equation row = lifted(record.steps[index].equation);
        row.plus_assign(original_false);
        const Hint hint = trail.cite(std::span<const Equation>(&row, 1));
        assert(hint != Hint::known && hint != Hint::not_unit &&
               "an earlier step's unit is no use to the line citing it");
        hints.push_back(step_line[index]);
        return hint == Hint::conflicting;
    };
    const auto replay = [&](std::span<const ClauseId> arcs) {
        const Citations cited = replay_arcs(arcs, trail);
        hints.insert(hints.end(), cited.hints.begin(), cited.hints.end());
        return cited.conflicted;
    };

    for (std::size_t index = 0; index < record.steps.size(); ++index) {
        const ImplicationGraphProofStep& step = record.steps[index];
        const std::span<const ClauseId> arcs(record.arcs.data() + step.arcs_begin,
                                             step.arcs_end - step.arcs_begin);
        Equation negated = lifted(step.equation);
        negated.plus_assign(original_false);

        rows.clear();
        hints.clear();
        [[maybe_unused]] bool conflicted = false;
        switch (step.kind) {
            case Kind::refuted:
                // Assume what the run refuted, and follow it to `0 = 1`.
                rows.push_back(negated);
                trail.restart(rows);
                conflicted = replay(arcs);
                break;
            case Kind::reaches_refuted:
                rows.push_back(negated);
                trail.restart(rows);
                conflicted = replay(arcs) || cite_step(step.source);
                break;
            case Kind::one_side:
                rows.push_back(negated);
                trail.restart(rows);
                conflicted = cite_step(step.source) || replay(arcs);
                break;
            case Kind::both_sides: {
                // Neither side is refuted, so the two halves of the case split
                // are clauses of their own, each cited once.
                const Equation assumed = lifted(step.assumed);
                Equation negated_assumed = assumed;
                negated_assumed.plus_assign(original_false);
                const std::span<const ClauseId> other_arcs(
                    record.arcs.data() + step.other_arcs_begin,
                    step.other_arcs_end - step.other_arcs_begin);

                const std::vector<Equation> first_rows{assumed, negated};
                const std::vector<Equation> second_rows{negated_assumed, negated};

                rows = first_rows;
                trail.restart(rows);
                [[maybe_unused]] const bool first = replay(arcs);
                assert(first && "one half of a two-sided unit reaches no conflict");
                const ClauseId first_line = write();

                rows = second_rows;
                hints.clear();
                trail.restart(rows);
                [[maybe_unused]] const bool second = replay(other_arcs);
                assert(second && "the other half of a two-sided unit reaches no conflict");
                const ClauseId second_line = write();

                // Without the equation, one half leaves the negation of what the
                // other half then falsifies.
                rows = {negated};
                hints = {first_line, second_line};
                trail.restart(rows);
                conflicted = trail.cite(first_rows) == Hint::propagated &&
                             trail.cite(second_rows) == Hint::conflicting;
                break;
            }
        }
        assert(conflicted && "a unit the implication graph found has no replay that conflicts");
        step_line[index] = write();
        if (step.fed) {
            unit_lines.push_back(step_line[index]);
        }
    }
    return unit_lines;
}

} // namespace xorcle
