#pragma once

/// @file saved_trails.h
/// @brief Phase saving over trails: the target trail and the recent trails, and
/// choosing a decision's space by them.

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "algebra/equation.h"
#include "algebra/equation_repr.h"
#include "algebra/row_echelon_matrix.h"
#include "core/clause_id.h"
#include "core/trail.h"
#include "heuristics/restarts/stabilize.h"

namespace xorcle {

struct PhaseSavingConfig {
    std::size_t recent_trails = 1; ///< Most recent conflict trails kept.
    bool target = true;
    std::size_t target_reset_conflicts = 1000; ///< Conflicts between resets of the target trail.
    bool target_on_ties = false; ///< Replace the target by a trail as long as it, too.
    /// @brief Conflicts in each step of the first cycle of `PhaseRotation`; none for the fixed
    /// order: the target trail, then the recent trails, then a uniformly drawn side.
    std::optional<std::size_t> rotation_interval = 1000;

    bool operator==(const PhaseSavingConfig&) const = default;
};

/// @brief The orders `--phase=rotate` consults the target trail (T), the recent trails (R)
/// and the falsify side (F) in, in the order of their cycle.
enum class PhaseOrder { target_recent_falsify, falsify, target_falsify, recent_falsify };

inline constexpr std::size_t num_phase_orders = 4;

/// @brief The cycle of `PhaseOrder`s, every step of cycle `c` (from 1) lasting `c` intervals
/// of conflicts.
class PhaseRotation {
public:
    /// @pre `interval_conflicts > 0`.
    explicit PhaseRotation(std::size_t interval_conflicts);

    /// @brief Count a conflict, and return whether it ended the step.
    bool update_with_conflict() noexcept;

    PhaseOrder order() const noexcept { return static_cast<PhaseOrder>(step_idx_); }

private:
    std::size_t interval_conflicts_;
    std::size_t cycle_ = 1;
    std::size_t step_idx_ = 0;
    std::size_t conflicts_in_step_ = 0;
};

/// @brief How one kind of saved trail fared at the clause step.
struct ClauseStepCounters {
    std::size_t accepted = 0;
    std::size_t trivial = 0; ///< Clauses rejected for an intersection inside `span^±(U)`.
    std::size_t inconsistent = 0; ///< Clauses rejected for an intersection holding `0 = 1` with U.
};

struct PhaseSavingCounters {
    std::size_t decisions = 0;
    ClauseStepCounters target_clause_step;
    ClauseStepCounters recent_clause_step;
    std::size_t clause_step_top_clause = 0; ///< Clause steps accepting the top clause.
    std::size_t clause_step_partner = 0; ///< Clause steps accepting a partner.
    std::size_t target_narrowing = 0;
    std::size_t recent_narrowing = 0;
    std::size_t none = 0; ///< Decisions neither step changed.
    std::size_t uniform_sides = 0; ///< Decisions whose side is drawn uniformly.
    std::size_t falsify_sides = 0; ///< Decisions whose side is the falsify side, by rotation.
    std::array<std::size_t, num_phase_orders> order_decisions{}; ///< By rotation, per order.
    std::size_t rotations = 0;
    std::size_t dimension_before = 0; ///< Directions of the decision's space modulo U.
    std::size_t dimension_after = 0;
    std::size_t cleared = 0; ///< Times a substitution or renumbering discarded every saved trail.
    double time_s = 0.0;
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
    /// Under a rotation, the trails are consulted in its order, and a side left open is
    /// the falsify side.
    /// @pre `trail_span` is the span of the trail `update_with_conflict()` last saved from,
    /// as it now stands, `level_stamps` is not empty, `space` holds a row, and
    /// `consults_trails()`.
    void narrow(RowSpace& space, std::span<const ClauseId> clause_ids,
                const std::vector<Clause>& clauses, const RowSpace& trail_span,
                std::span<const std::size_t> level_stamps);

    /// @brief Whether `narrow()` chooses the next decision's space: false only when the
    /// rotation's order consults no trail that is kept.
    bool consults_trails() const noexcept;

    /// @brief Count a decision of the rotation's falsify order, its side the falsify side.
    /// @pre `consults_trails()` is false.
    void note_falsify_decision();

    /// @brief Consult the target trail only in stable mode; it is saved in both.
    void update_with_mode(SearchMode mode) noexcept { mode_ = mode; }

    /// @brief Forget every saved trail, whose rows a substitution or renumbering invalidates.
    void clear();

    /// @brief Forget every saved trail and adopt a new variable count.
    void reconfigure(std::size_t num_vars);

    /// @brief Forget everything, the counters included.
    void reinitialize(std::size_t num_vars);

    const PhaseSavingCounters& counters() const noexcept { return counters_; }

private:
    struct SavedTrail {
        std::vector<Equation> rows;
        /// @brief `level_ends[k]` is how many of `rows` lie at levels `k + 1` or below.
        std::vector<std::size_t> level_ends;
        std::vector<std::size_t> level_stamps; ///< Of levels `0` to `level_ends.size()`.
        /// @brief Rows at the root when saved, the first rows of the current root.
        std::size_t root_rows = 0;

        /// @brief The span of the saved trail, its root included, built when first needed.
        std::optional<RowSpace> span;
        /// @brief The current trail's rows reduced by `span`, one entry per trail row
        /// in trail order: the pivot column the row took, or none when it added nothing.
        std::optional<RowSpace> trail_mod_span;
        std::vector<std::optional<std::size_t>> trail_mod_span_pivot_col_idxs;
        std::vector<std::size_t> trail_mod_span_stamps;
        std::vector<std::size_t> trail_mod_span_ends;

        std::size_t num_rows() const noexcept { return level_ends.empty() ? 0 : level_ends.back(); }
    };

    enum class ClauseStep { accepted, trivial, inconsistent };

    /// @brief Copy the levels `1` to `num_levels` of `trail`, up to trail position `end`, into `saved`.
    void save(SavedTrail& saved, const Trail& trail, std::span<const std::size_t> level_stamps,
              std::size_t num_levels, std::size_t end);

    /// @brief Discard the spaces built from `saved`'s rows.
    void drop_spans(SavedTrail& saved);

    /// @brief Build `saved.span` if it is not built.
    void build_span(SavedTrail& saved);

    /// @brief Put `span^±(V) ∩ span^±(neg(clause)) ∩ span(saved)` modulo `trail_span`
    /// in `candidate_`, with `space` holding `span^±(V)` modulo `trail_span`.
    ClauseStep clause_step(const RowSpace& space, const Clause& clause, SavedTrail& saved,
                           const RowSpace& trail_span);

    /// @brief Put `span^±(V) ∩ span(saved)` modulo the trail span in `candidate_`, with
    /// `space` holding `span^±(V)` modulo the trail span.
    void narrowing(const RowSpace& space, SavedTrail& saved,
                   std::span<const std::size_t> level_stamps);

    /// @brief Bring `saved.trail_mod_span` up to the current trail, dropping the rows of
    /// the trail positions that changed and reducing the rows from there on.
    void update_trail_mod_span(SavedTrail& saved, std::span<const std::size_t> level_stamps);

    /// @brief Replace `rows` by combinations of them spanning those that `residue`, a
    /// linear map applied in place, sends to `0 = 0`.
    template <typename Residue>
    void keep_kernel(std::vector<Equation>& rows, const Residue& residue);

    /// @brief Echelonize `rows` into `candidate_`, emptying `rows`.
    void set_candidate(std::vector<Equation>& rows);

    /// @brief A copy of `source` in a spare row.
    Equation spare_copy(const Equation& source);

    /// @brief Move every row of `space` to the spare rows.
    void recycle_rows(RowSpace& space);

    EquationRepr repr_;
    std::size_t num_vars_;
    PhaseSavingConfig config_;

    SavedTrail target_;
    std::size_t conflicts_since_target_reset_ = 0;
    std::optional<PhaseRotation> rotation_;
    SearchMode mode_ = SearchMode::stable;
    std::vector<SavedTrail> recent_; ///< Most recent first.
    /// @brief The saved trails holding rows, the target first, and whether each is the target.
    std::vector<std::pair<SavedTrail*, bool>> trails_;

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
    std::vector<Equation> spare_rows_;

    PhaseSavingCounters counters_;
    /// @brief The trail of the last conflict, which is the trail `narrow()` sees.
    const Trail* trail_ = nullptr;
};

} // namespace xorcle
