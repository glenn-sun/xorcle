#include "heuristics/decisions/saved_trails.h"

#include <algorithm>
#include <cassert>
#include <utility>

namespace xorcle {

namespace {

bool uses_target(PhaseSaving source) {
    return source == PhaseSaving::both || source == PhaseSaving::target_only ||
           source == PhaseSaving::rotate;
}

bool uses_recent(PhaseSaving source) {
    return source == PhaseSaving::both || source == PhaseSaving::recent_only ||
           source == PhaseSaving::rotate;
}

bool holds_false(const RowSpace& space) {
    return space.pivot_col_idxs().test(space.cols() - 1);
}

std::size_t directions(const RowSpace& space) {
    return space.rows() - (holds_false(space) ? 1 : 0);
}

#ifndef NDEBUG
/// @brief The span of `rows` modulo `trail_span`, in reduced row-echelon form.
RowSpace modulo(const std::vector<Equation>& rows, const RowSpace& trail_span) {
    RowSpace image(trail_span.repr(), trail_span.num_vars());
    for (Equation row : rows) {
        trail_span.reduce(row);
        image.add_row(std::move(row));
    }
    image.reduce_rows();
    return image;
}

bool same_span(RowSpace left, RowSpace right) {
    left.reduce_rows();
    right.reduce_rows();
    return left.rows() == right.rows() && left.has_rows_of(right);
}
#endif

} // namespace

PhaseRotation::PhaseRotation(std::size_t interval_conflicts)
    : interval_conflicts_(interval_conflicts) {
    assert(interval_conflicts > 0 && "a rotation step lasts at least one conflict");
}

bool PhaseRotation::update_with_conflict() noexcept {
    if (++conflicts_in_step_ < cycle_ * interval_conflicts_) {
        return false;
    }
    conflicts_in_step_ = 0;
    if (++step_idx_ == rotation_steps.size()) {
        step_idx_ = 0;
        ++cycle_;
    }
    return true;
}

SavedTrails::SavedTrails(EquationRepr repr, std::size_t num_vars, const PhaseSavingConfig& config)
    : repr_(repr),
      num_vars_(num_vars),
      config_(config),
      keeps_target_(uses_target(config.source) ||
                    (config.focused_source.has_value() && uses_target(*config.focused_source))),
      keeps_recent_(uses_recent(config.source) ||
                    (config.focused_source.has_value() && uses_recent(*config.focused_source))),
      candidate_(repr, num_vars),
      work_row_(repr, num_vars),
      spare_rows_(repr, num_vars + 1) {
    if (config.source == PhaseSaving::rotate || config.focused_source == PhaseSaving::rotate) {
        rotation_.emplace(config.rotation_interval);
    }
}


void SavedTrails::note_unconsulted_decision() {
    assert(!consults_trails() && "a decision a saved trail was consulted for is counted by narrow()");
    if (mode_source() == PhaseSaving::rotate) {
        ++counters_.rotation_step_decisions[rotation_->step_idx()];
    }
    ++counters_.open_sides;
}

void SavedTrails::reset_workspace(std::size_t num_vars) {
    num_vars_ = num_vars;
    candidate_ = RowSpace(repr_, num_vars);
    work_row_ = Equation(repr_, num_vars);
    kernel_rows_.clear();
    kernel_kept_.clear();
    kernel_residues_.clear();
    kernel_combinations_.clear();
    spare_rows_.clear(num_vars + 1);
}

void SavedTrails::reinitialize(std::size_t num_vars) {
    target_ = {};
    recent_ = {};
    reset_workspace(num_vars);
    conflicts_since_target_reset_ = 0;
    if (rotation_.has_value()) {
        rotation_.emplace(config_.rotation_interval);
    }
    counters_ = {};
    trail_ = nullptr;
}

void SavedTrails::update_with_substitution(const RowSpace& root_span,
                                           [[maybe_unused]] const IndexSet& dropped_col_idxs) {
    rewrite(target_, root_span);
    rewrite(recent_, root_span);
#ifndef NDEBUG
    const auto carries_no_dropped_variable = [&dropped_col_idxs](const SavedTrail& saved) {
        for (std::size_t row_idx = 0; row_idx < saved.num_rows(); ++row_idx) {
            for (const std::size_t bit : saved.rows[row_idx].bits()) {
                if (dropped_col_idxs.test(bit)) {
                    return false;
                }
            }
        }
        return true;
    };
    assert(carries_no_dropped_variable(target_) &&
           carries_no_dropped_variable(recent_) &&
           "a rewritten saved row carries a dropped variable");
#endif
}

void SavedTrails::update_with_renumbering(const CompactionTransform& transform) {
    assert(transform.old_num_vars() == num_vars_ &&
           "the transform was built for a different active variable numbering");
    const auto renumber = [&transform, this](SavedTrailWithSpans& saved) {
        if (saved.trail_mod_span.has_value()) {
            spare_rows_.give_rows_of(*saved.trail_mod_span);
            saved.trail_mod_span.reset();
        }
        saved.renumber(transform);
        if (saved.span.has_value()) {
            saved.span->compact(transform);
        }
    };
    renumber(target_);
    renumber(recent_);
    reset_workspace(transform.new_num_vars());
}

void SavedTrails::rewrite(SavedTrailWithSpans& saved, const RowSpace& root_span) {
    drop_spans(saved);
    if (saved.num_rows() == 0) {
        saved.level_ends.clear();
        return;
    }
    saved.substitute(root_span);
    saved.span.emplace(repr_, num_vars_);
    RowSpace& span = *saved.span;
    std::size_t num_kept = 0;
    std::size_t row_idx = 0;
    for (std::size_t level_idx = 0; level_idx < saved.level_ends.size(); ++level_idx) {
        const std::size_t kept_below = num_kept;
        for (; row_idx < saved.level_ends[level_idx]; ++row_idx) {
            Equation row = spare_rows_.copy_of(saved.rows[row_idx]);
            if (!span.add_row_or_keep(row).has_value()) {
                spare_rows_.give(std::move(row));
                continue;
            }
            if (num_kept != row_idx) {
                std::swap(saved.rows[num_kept], saved.rows[row_idx]);
            }
            ++num_kept;
        }
        if (holds_false(span)) {
            // The levels below were consistent before this one came.
            num_kept = kept_below;
            saved.level_ends.resize(level_idx);
            saved.level_stamps.resize(level_idx + 1);
            spare_rows_.give_rows_of(span);
            for (std::size_t kept_idx = 0; kept_idx < num_kept; ++kept_idx) {
                span.add_row(spare_rows_.copy_of(saved.rows[kept_idx]));
            }
            break;
        }
        saved.level_ends[level_idx] = num_kept;
    }
    assert(!holds_false(span) && "a rewritten saved trail holds 0 = 1");
}

void SavedTrails::drop_spans(SavedTrailWithSpans& saved) {
    for (std::optional<RowSpace>* space : {&saved.span, &saved.trail_mod_span}) {
        if (space->has_value()) {
            spare_rows_.give_rows_of(**space);
            space->reset();
        }
    }
}

void SavedTrails::update_with_conflict(const Trail& trail,
                                       std::span<const std::size_t> level_stamps) {
    const std::size_t level = trail.level();
    assert(level > 0 && level_stamps.size() > level && "a non-root conflict has a stamp per level");
    assert(trail.span().pivot_col_idxs().test(trail.span().cols() - 1) &&
           "a conflict's trail ends in its 0 = 1");
    trail_ = &trail;
    if (rotation_.has_value()) {
        rotation_->update_with_conflict();
    }
    if (keeps_recent_) {
        // Its last row is the conflict's `0 = 1`.
        recent_.save(trail, level_stamps, level, trail.rows_at_level(level) - 1);
        drop_spans(recent_);
    }
    if (keeps_target_) {
        if (++conflicts_since_target_reset_ >= config_.target_reset_conflicts) {
            conflicts_since_target_reset_ = 0;
            target_.level_ends.clear();
            drop_spans(target_);
        }
        const std::size_t below = trail.rows_at_level(level - 1) - trail.rows_at_level(0);
        if (below > target_.num_rows()) {
            target_.save(trail, level_stamps, level - 1, trail.rows_at_level(level - 1));
            drop_spans(target_);
        }
    }
}

void SavedTrails::narrow(RowSpace& space, std::span<const ClauseId> clause_ids,
                         const std::vector<Clause>& clauses, const RowSpace& trail_span,
                         std::span<const std::size_t> level_stamps) {
    assert(!level_stamps.empty() && space.rows() > 0 && !holds_false(space) &&
           "phase saving narrows a nonempty space without 0 = 1, knowing the level stamps");
    assert(consults_trails() && "phase saving narrows only when a saved trail is consulted");
    if (mode_source() == PhaseSaving::rotate) {
        ++counters_.rotation_step_decisions[rotation_->step_idx()];
    }
    Equation false_row = spare_rows_.copy_of(space.row(0));
    false_row.set_eq_false();
    space.add_row(std::move(false_row));

    std::vector<SavedTrailWithSpans*>& trails = trails_;
    trails.clear();
    const PhaseSaving consulted = source();
    if (uses_target(consulted) && target_.num_rows() > 0) {
        trails.push_back(&target_);
    }
    if (uses_recent(consulted) && recent_.num_rows() > 0) {
        trails.push_back(&recent_);
    }
    assert((trails.empty() || (trail_ != nullptr && &trail_->span() == &trail_span)) &&
           "phase saving narrows against the trail it saves from");

    bool decided = false;
    for (SavedTrailWithSpans* const saved : trails) {
        for (std::size_t clause_idx = 0; clause_idx < clause_ids.size(); ++clause_idx) {
            const ClauseStep result =
                clause_step(space, clauses[clause_ids[clause_idx]], *saved, trail_span);
            if (result == ClauseStep::accepted) {
                decided = true;
                break;
            }
        }
        if (decided) {
            break;
        }
    }
    if (!decided) {
        for (SavedTrailWithSpans* const saved : trails) {
            narrowing(space, *saved, level_stamps);
            if (directions(candidate_) > 0) {
                decided = true;
                break;
            }
        }
    }
    if (decided) {
        std::swap(space, candidate_);
    }
    spare_rows_.give_rows_of(candidate_);

    // The draw depends on the basis, so the space is drawn from in its canonical form.
    space.reduce_rows();
    if (holds_false(space)) {
        ++counters_.open_sides;
    }
}

SavedTrails::ClauseStep SavedTrails::clause_step(const RowSpace& space, const Clause& clause,
                                                 SavedTrailWithSpans& saved,
                                                 const RowSpace& trail_span) {
    build_span(saved);
    // `span^±(neg(C)) ∩ span(S)`, then its rows modulo U, and of those the combinations
    // in `span^±(V)` modulo U, which `space` holds.
    kernel_rows_.clear();
    for (const Equation& row : clause) {
        kernel_rows_.push_back(spare_rows_.copy_of(row));
    }
    kernel_rows_.push_back(spare_rows_.copy_of(clause.row(0)));
    kernel_rows_.back().set_eq_false();
    keep_kernel(kernel_rows_, [&saved](Equation& row) { saved.span->reduce(row); });
    for (Equation& row : kernel_rows_) {
        trail_span.reduce(row);
    }
    keep_kernel(kernel_rows_, [&space](Equation& row) { space.reduce(row); });
    set_candidate(kernel_rows_);
#ifndef NDEBUG
    RowSpace pm_space = trail_span;
    for (const Equation& row : space) {
        pm_space.add_row(row);
    }
    RowSpace pm_clause(repr_, num_vars_);
    for (const Equation& row : clause) {
        pm_clause.add_row(row);
    }
    pm_clause.add_row(Equation::eq_false(repr_, num_vars_));
    RowSpace saved_span(repr_, num_vars_);
    for (std::size_t trail_idx = 0; trail_idx < saved.root_rows; ++trail_idx) {
        saved_span.add_row(trail_->row(trail_idx));
    }
    for (std::size_t row_idx = 0; row_idx < saved.num_rows(); ++row_idx) {
        saved_span.add_row(saved.rows[row_idx]);
    }
    RowSpace meet(repr_, num_vars_);
    for (Equation& row : span_intersection(pm_space, pm_clause)) {
        meet.add_row(std::move(row));
    }
    assert(same_span(modulo(span_intersection(meet, saved_span), trail_span), candidate_) &&
           "the clause step's intersection is wrong");
#endif
    if (directions(candidate_) == 0) {
        return ClauseStep::trivial;
    }
    return holds_false(candidate_) ? ClauseStep::inconsistent : ClauseStep::accepted;
}

void SavedTrails::narrowing(const RowSpace& space, SavedTrailWithSpans& saved,
                            std::span<const std::size_t> level_stamps) {
    update_trail_mod_span(saved, level_stamps);
    // A combination of the space's rows lies in the span of the saved trail and the
    // current one exactly when its reduction by the saved span and then by the trail's
    // rows reduced by it is zero.
    kernel_rows_.clear();
    for (const Equation& row : space) {
        kernel_rows_.push_back(spare_rows_.copy_of(row));
    }
    keep_kernel(kernel_rows_, [&saved](Equation& row) {
        saved.span->reduce(row);
        saved.trail_mod_span->reduce(row);
    });
    set_candidate(kernel_rows_);
#ifndef NDEBUG
    RowSpace full = trail_->span();
    for (std::size_t row_idx = 0; row_idx < saved.num_rows(); ++row_idx) {
        full.add_row(saved.rows[row_idx]);
    }
    RowSpace expected(repr_, num_vars_);
    for (Equation& row : span_intersection(space, full)) {
        expected.add_row(std::move(row));
    }
    assert(same_span(expected, candidate_) && "the direction narrowing's intersection is wrong");
#endif
}

void SavedTrails::build_span(SavedTrailWithSpans& saved) {
    if (saved.span.has_value()) {
        return;
    }
    // The saved rows and the root's are stored trail rows, which lead in distinct
    // columns; added in column order, each goes in last.
    const Trail& trail = *trail_;
    assert(trail.rows_at_level(0) >= saved.root_rows && "the root lost rows a saved trail had");
    saved.span.emplace(repr_, num_vars_);
    span_order_.clear();
    for (std::size_t trail_idx = 0; trail_idx < saved.root_rows; ++trail_idx) {
        span_order_.emplace_back(*trail.row(trail_idx).leading_bit(),
                                 saved.num_rows() + trail_idx);
    }
    for (std::size_t row_idx = 0; row_idx < saved.num_rows(); ++row_idx) {
        span_order_.emplace_back(*saved.rows[row_idx].leading_bit(), row_idx);
    }
    std::ranges::sort(span_order_);
    for (const auto& [leading_col_idx, row_idx] : span_order_) {
        saved.span->add_row(spare_rows_.copy_of(row_idx < saved.num_rows()
                                           ? saved.rows[row_idx]
                                           : trail.row(row_idx - saved.num_rows())));
    }
    assert(!holds_false(*saved.span) && "a saved trail holds 0 = 1");
}

void SavedTrails::update_trail_mod_span(SavedTrailWithSpans& saved,
                                        std::span<const std::size_t> level_stamps) {
    const Trail& trail = *trail_;
    build_span(saved);
    if (!saved.trail_mod_span.has_value()) {
        saved.trail_mod_span.emplace(repr_, num_vars_);
        saved.trail_mod_span_pivot_col_idxs.clear();
        saved.trail_mod_span_stamps.clear();
        saved.trail_mod_span_ends.clear();
    }
    // The first trail position whose row changed: rows at a level stay while its
    // stamp does, and a row learned at a lower level goes to that level's end.
    std::size_t cut = saved.trail_mod_span_pivot_col_idxs.size();
    const std::size_t level = level_stamps.size() - 1;
    const std::size_t kept_levels = saved.trail_mod_span_stamps.size();
    for (std::size_t kept = 0; kept < kept_levels; ++kept) {
        if (kept > level || level_stamps[kept] != saved.trail_mod_span_stamps[kept]) {
            cut = kept == 0 ? 0 : saved.trail_mod_span_ends[kept - 1];
            break;
        }
        if (kept + 1 < kept_levels &&
            trail.rows_at_level(kept) != saved.trail_mod_span_ends[kept]) {
            cut = saved.trail_mod_span_ends[kept];
            break;
        }
    }
    RowSpace& reduced = *saved.trail_mod_span;
    while (saved.trail_mod_span_pivot_col_idxs.size() > cut) {
        const std::optional<std::size_t> pivot_col_idx = saved.trail_mod_span_pivot_col_idxs.back();
        saved.trail_mod_span_pivot_col_idxs.pop_back();
        if (!pivot_col_idx.has_value()) {
            continue;
        }
        // The last row added is the one to go.
        const std::size_t row_idx = reduced.row_idx_for_col_idx(*pivot_col_idx);
        assert(row_idx < reduced.rows() && reduced.pivot_col_idx(row_idx) == *pivot_col_idx &&
               "a kept row is missing");
        spare_rows_.give(reduced.pop_row(row_idx));
    }
    // A row of a level shared with the saved trail, among the rows that level had when
    // saved, is a saved row plus rows before it, so it adds nothing to the reduced rows.
    std::vector<std::size_t>& shared_ends = shared_level_ends_;
    shared_ends.clear();
    for (std::size_t shared = 1; shared <= std::min(level, saved.level_ends.size()) &&
                                 level_stamps[shared] == saved.level_stamps[shared];
         ++shared) {
        const std::size_t saved_rows =
            saved.level_ends[shared - 1] - (shared == 1 ? 0 : saved.level_ends[shared - 2]);
        shared_ends.push_back(trail.rows_at_level(shared - 1) + saved_rows);
    }
    for (std::size_t trail_idx = cut; trail_idx < trail.span().rows(); ++trail_idx) {
        const std::size_t row_level = trail.level_of(trail_idx);
        if (row_level >= 1 && row_level <= shared_ends.size() &&
            trail_idx < shared_ends[row_level - 1]) {
            saved.trail_mod_span_pivot_col_idxs.emplace_back(std::nullopt);
            continue;
        }
        work_row_.assign_from(trail.row(trail_idx));
        saved.span->reduce(work_row_);
        Equation row = spare_rows_.copy_of(work_row_);
        const std::optional<std::size_t> row_idx = reduced.add_row_or_keep(row);
        if (row_idx.has_value()) {
            saved.trail_mod_span_pivot_col_idxs.emplace_back(reduced.pivot_col_idx(*row_idx));
        } else {
            saved.trail_mod_span_pivot_col_idxs.emplace_back(std::nullopt);
            spare_rows_.give(std::move(row));
        }
    }
    saved.trail_mod_span_stamps.assign(level_stamps.begin(), level_stamps.end());
    saved.trail_mod_span_ends.resize(level + 1);
    for (std::size_t kept = 0; kept <= level; ++kept) {
        saved.trail_mod_span_ends[kept] = trail.rows_at_level(kept);
    }
}

template <typename Residue>
void SavedTrails::keep_kernel(std::vector<Equation>& rows, const Residue& residue) {
    // Each row is carried along with its residue, and the rows whose residues
    // eliminate to zero are kept.
    kernel_leads_.clear();
    kernel_kept_.clear();
    for (Equation& row : rows) {
        Equation residual = spare_rows_.copy_of(row);
        residue(residual);
        std::optional<std::size_t> lead = residual.leading_bit();
        for (; lead.has_value(); lead = residual.leading_bit()) {
            const auto found = std::ranges::find(kernel_leads_, *lead);
            if (found == kernel_leads_.end()) {
                break;
            }
            const auto basis_idx = static_cast<std::size_t>(found - kernel_leads_.begin());
            residual.plus_assign(kernel_residues_[basis_idx]);
            row.plus_assign(kernel_combinations_[basis_idx]);
        }
        if (lead.has_value()) {
            kernel_leads_.push_back(*lead);
            kernel_residues_.push_back(std::move(residual));
            kernel_combinations_.push_back(std::move(row));
        } else {
            spare_rows_.give(std::move(residual));
            kernel_kept_.push_back(std::move(row));
        }
    }
    for (std::vector<Equation>* basis : {&kernel_residues_, &kernel_combinations_}) {
        for (Equation& row : *basis) {
            spare_rows_.give(std::move(row));
        }
        basis->clear();
    }
    rows.swap(kernel_kept_);
    kernel_kept_.clear();
}

void SavedTrails::set_candidate(std::vector<Equation>& rows) {
    spare_rows_.give_rows_of(candidate_);
    for (Equation& row : rows) {
        if (!candidate_.add_row_or_keep(row).has_value()) {
            spare_rows_.give(std::move(row));
        }
    }
    rows.clear();
}

} // namespace xorcle
