#pragma once

/// @file saved_trails.h
/// @brief Phase saving over trails: the target trail and the recent trail, and
/// choosing a decision's space by them.

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "algebra/equation.h"
#include "algebra/equation_pool.h"
#include "algebra/equation_repr.h"
#include "algebra/row_echelon_matrix.h"
#include "algebra/compaction_transform.h"
#include "core/clause_id.h"
#include "core/saved_trail.h"
#include "core/trail.h"
#include "util/index_set.h"
#include "heuristics/restarts/stabilize.h"

namespace xorcle {

/// @brief The saved trails a decision's space is narrowed by.
enum class PhaseSaving {
    both, ///< The target trail, then the recent trail.
    target_only,
    recent_only,
    none,
    /// `both`, `none`, `target_only` and `recent_only` in turn, as `PhaseRotation` steps.
    rotate
};

struct PhaseSavingConfig {
    PhaseSaving source;
    /// @brief Conflicts between resets of the target trail, when `source` uses it.
    std::size_t target_reset_conflicts;
    /// @brief Conflicts in each step of the first cycle of `PhaseRotation`, under `rotate`.
    std::size_t rotation_interval;
    /// @brief The source in focused mode, present exactly when the search has one.
    std::optional<PhaseSaving> focused_source;
};

/// @brief The steps of `PhaseSaving::rotate`, in the order of their cycle.
inline constexpr std::array<PhaseSaving, 4> rotation_steps{
    PhaseSaving::both, PhaseSaving::none, PhaseSaving::target_only, PhaseSaving::recent_only};

/// @brief The cycle of `rotation_steps`, every step of cycle `c` (from 1) lasting `c`
/// intervals of conflicts.
class PhaseRotation {
public:
    /// @pre `interval_conflicts > 0`.
    explicit PhaseRotation(std::size_t interval_conflicts);

    /// @brief Count a conflict, and return whether it ended the step.
    bool update_with_conflict() noexcept;

    std::size_t step_idx() const noexcept { return step_idx_; }
    PhaseSaving source() const noexcept { return rotation_steps[step_idx_]; }

private:
    std::size_t interval_conflicts_;
    std::size_t cycle_ = 1;
    std::size_t step_idx_ = 0;
    std::size_t conflicts_in_step_ = 0;
};

struct PhaseSavingCounters {
    std::size_t open_sides = 0; ///< Decisions whose side is left to `DecisionPhase`.
    /// @brief Decisions under each of `rotation_steps`.
    std::array<std::size_t, rotation_steps.size()> rotation_step_decisions{};
};

/// @brief The trails phase saving keeps, and the choice of a decision's space by them.
///
/// A saved trail holds its rows above the root in trail order, with the stamp of
/// each level, so that the levels it shares with the current trail are known. See
/// arXiv:2605.15002 for CSIDS, whose intersection V this narrows.
class SavedTrails {
public:
    SavedTrails(EquationRepr repr, std::size_t num_vars, const PhaseSavingConfig& config);

    /// @brief Save the trail of a conflict, before the backjump, without its `0 = 1`.
    /// @param level_stamps The stamp of every level of `trail`.
    /// @pre `trail`'s last row is the conflict's `0 = 1`.
    void update_with_conflict(const Trail& trail, std::span<const std::size_t> level_stamps);

    /// @brief Replace `space`, V modulo `trail_span`, by the space a decision is drawn
    /// from: `span^±(V)` narrowed by the clause step, or else by direction narrowing.
    ///
    /// The clause step takes, for the first saved trail S and the first of `clauses`
    /// that make it nontrivial and consistent with the trail span U,
    /// `span^±(V) ∩ span^±(neg(C)) ∩ span(S)`; direction narrowing takes
    /// `span^±(V) ∩ span(S)` for the first S that leaves it nontrivial.
    /// @param space Rows reduced by `trail_span`, without `0 = 1`; on return, the same,
    /// with `0 = 1` as the last row when the side is open.
    /// @param clause_ids The clauses that changed V, the top clause first, in decreasing activity.
    /// @param level_stamps The stamp of every level of the current trail.
    /// @pre `trail_span` is the span of the trail `update_with_conflict()` last saved from,
    /// as it now stands, `level_stamps` is not empty, `space` holds a row, and
    /// `consults_trails()`.
    void narrow(RowSpace& space, std::span<const ClauseId> clause_ids,
                const std::vector<Clause>& clauses, const RowSpace& trail_span,
                std::span<const std::size_t> level_stamps);

    /// @brief Whether any trail is saved.
    bool keeps_trails() const noexcept { return keeps_target_ || keeps_recent_; }

    /// @brief Whether `narrow()` chooses the next decision's space.
    bool consults_trails() const noexcept { return source() != PhaseSaving::none; }

    /// @brief Count a decision whose side no saved trail was consulted for.
    /// @pre `consults_trails()` is false.
    void note_unconsulted_decision();

    /// @brief Consult the trails of the mode's source.
    void update_with_mode(SearchMode mode) noexcept { mode_ = mode; }

    /// @brief Rewrite every saved trail for a substitution of the root rows `root_span`,
    /// cutting each before its first level whose span with the rows below holds `0 = 1`.
    /// @param dropped_col_idxs Every variable dropped since the last renumbering, which no
    /// rewritten row carries.
    /// @pre `root_span` holds the root rows of the trail `update_with_conflict()` saved from.
    void update_with_substitution(const RowSpace& root_span, const IndexSet& dropped_col_idxs);

    /// @brief Renumber every saved trail, and adopt the new variable count.
    /// @pre `update_with_substitution()` has just run.
    void update_with_renumbering(const CompactionTransform& transform);

    /// @brief Forget everything, the counters included.
    void reinitialize(std::size_t num_vars);

    const PhaseSavingCounters& counters() const noexcept { return counters_; }

private:
    /// @brief A saved trail with the spaces built from it.
    struct SavedTrailWithSpans : SavedTrail {
        /// @brief The span of the saved trail, its root included, built when first needed.
        std::optional<RowSpace> span;
        /// @brief The current trail's rows reduced by `span`, one entry per trail row
        /// in trail order: the pivot column the row took, or none when it added nothing.
        std::optional<RowSpace> trail_mod_span;
        std::vector<std::optional<std::size_t>> trail_mod_span_pivot_col_idxs;
        std::vector<std::size_t> trail_mod_span_stamps;
        std::vector<std::size_t> trail_mod_span_ends;
    };

    enum class ClauseStep { accepted, trivial, inconsistent };

    /// @brief The source of the current mode.
    PhaseSaving mode_source() const noexcept {
        return mode_ == SearchMode::focused && config_.focused_source.has_value()
                   ? *config_.focused_source
                   : config_.source;
    }

    /// @brief The trails the next decision consults: the mode's source, or under
    /// `rotate` the rotation's step.
    PhaseSaving source() const noexcept {
        return mode_source() == PhaseSaving::rotate ? rotation_->source() : mode_source();
    }

    /// @brief Discard the spaces built from `saved`'s rows.
    void drop_spans(SavedTrailWithSpans& saved);

    /// @brief Build `saved.span` if it is not built.
    void build_span(SavedTrailWithSpans& saved);

    /// @brief Rewrite `saved` for a substitution of `root_span`, keep only the rows that add
    /// to the span of those before, and cut it before the first level that brings `0 = 1`.
    void rewrite(SavedTrailWithSpans& saved, const RowSpace& root_span);

    /// @brief Size the workspace for `num_vars` variables, emptying it.
    void reset_workspace(std::size_t num_vars);

    /// @brief Put `span^±(V) ∩ span^±(neg(clause)) ∩ span(saved)` modulo `trail_span`
    /// in `candidate_`, with `space` holding `span^±(V)` modulo `trail_span`.
    ClauseStep clause_step(const RowSpace& space, const Clause& clause, SavedTrailWithSpans& saved,
                           const RowSpace& trail_span);

    /// @brief Put `span^±(V) ∩ span(saved)` modulo the trail span in `candidate_`, with
    /// `space` holding `span^±(V)` modulo the trail span.
    void narrowing(const RowSpace& space, SavedTrailWithSpans& saved,
                   std::span<const std::size_t> level_stamps);

    /// @brief Bring `saved.trail_mod_span` up to the current trail, dropping the rows of
    /// the trail positions that changed and reducing the rows from there on.
    void update_trail_mod_span(SavedTrailWithSpans& saved,
                               std::span<const std::size_t> level_stamps);

    /// @brief Replace `rows` by combinations of them spanning those that `residue`, a
    /// linear map applied in place, sends to `0 = 0`.
    template <typename Residue>
    void keep_kernel(std::vector<Equation>& rows, const Residue& residue);

    /// @brief Echelonize `rows` into `candidate_`, emptying `rows`.
    void set_candidate(std::vector<Equation>& rows);

    EquationRepr repr_;
    std::size_t num_vars_;
    PhaseSavingConfig config_;

    SavedTrailWithSpans target_;
    std::size_t conflicts_since_target_reset_ = 0;
    std::optional<PhaseRotation> rotation_;
    SearchMode mode_ = SearchMode::stable;
    bool keeps_target_;
    bool keeps_recent_;
    SavedTrailWithSpans recent_;
    /// @brief The saved trails holding rows, the target first.
    std::vector<SavedTrailWithSpans*> trails_;

    RowSpace candidate_;
    std::vector<Equation> kernel_rows_;
    std::vector<Equation> kernel_kept_;
    std::vector<Equation> kernel_residues_;
    std::vector<Equation> kernel_combinations_;
    std::vector<std::size_t> kernel_leads_;
    /// @brief Where each level shared with a saved trail ends its saved rows, by trail position.
    std::vector<std::size_t> shared_level_ends_;
    /// @brief The saved rows' leading columns and indices, sorted.
    std::vector<std::pair<std::size_t, std::size_t>> span_order_;
    Equation work_row_;
    EquationPool spare_rows_;

    PhaseSavingCounters counters_;
    /// @brief The trail of the last conflict, which is the trail `narrow()` sees.
    const Trail* trail_ = nullptr;
};

} // namespace xorcle
