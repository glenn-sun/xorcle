#pragma once

/// @file clause_selector.h
/// @brief Choosing which clause to propagate from next, and owning the clause database.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "algebra/compaction_transform.h"
#include "algebra/equation.h"
#include "algebra/row_echelon_matrix.h"
#include "core/clause_id.h"
#include "core/trail.h"
#include "core/watch.h"

namespace xorcle {

struct ClauseSelection {
    ClauseId id;
    std::optional<WatchType> trigger_hint;
};

enum class ClauseSelectorName {
    cyclic, ///< Round-robin over the clause database.
    leading_term_watchlist ///< Work queue driven by the leading terms of watched rows.
};

/// @brief How long a clause survives a deletion pass.
enum class ClauseTier : std::uint8_t {
    core, ///< Kept forever.
    tier2, ///< Demoted to `local` once enough conflicts go by unused.
    local ///< Evicted by key at a deletion pass, unless it is a starting clause.
};

/// @brief The LBD of a clause no conflict has resolved against yet, which is
/// where a starting clause begins: it is tiered by its first use, never evicted.
inline constexpr std::size_t unset_clause_lbd = static_cast<std::size_t>(-1);

/// @brief The LBD cutoffs a clause's tier is read off.
struct ClauseTierConfig {
    std::size_t core_lbd = 2; ///< At most this enters `core`.
    std::size_t tier2_lbd = 6; ///< At most this, and above `core_lbd`, enters `tier2`.
};

/// @brief What conflict analysis knows about a learned clause beyond its rows.
struct AssertingClauseInfo {
    std::size_t lbd;

    /// @brief The number of decision levels just *discarded*, which after a restart
    /// is neither the asserting level nor its complement.
    std::size_t removed_levels;
};

/// @brief Decides which clause the solver tries to propagate from next.
///
/// It is also the **authoritative clause store**: the solver keeps no clause vector of its
/// own, and a watchlist selector's indexes are a view of this one.
///
/// @pre Every accessor taking a clause id requires `id < num_clauses()`.
class ClauseSelector : public WatchUpdateHandler {
public:
    ClauseSelector() = default;

    ClauseSelector(const ClauseSelector&) = delete;
    ClauseSelector& operator=(const ClauseSelector&) = delete;

    ~ClauseSelector() override = default;

    std::size_t num_clauses() const noexcept { return clauses_.size(); }

    const std::vector<Clause>& clauses() const noexcept { return clauses_; }

    Clause& clause(ClauseId id);

    const Clause& clause(ClauseId id) const;
    bool is_clause_active(ClauseId id) const;
    void set_clause_active(ClauseId id, bool active);

    const std::vector<std::uint8_t>& active_flags() const noexcept { return clause_active_; }

    /// @brief Whether the clause was learned rather than read from the instance.
    bool is_clause_learned(ClauseId id) const;

    /// @brief `unset_clause_lbd` until a use records one.
    std::size_t clause_lbd(ClauseId id) const;
    void set_clause_lbd(ClauseId id, std::size_t lbd);

    ClauseTier clause_tier(ClauseId id) const;
    void set_clause_tier(ClauseId id, ClauseTier tier);

    bool is_clause_used_since_deletion(ClauseId id) const;
    void set_clause_used_since_deletion(ClauseId id, bool used);

    /// @brief Conflicts the clause has gone unused, counted at the deletion
    /// passes it sat through unlocked.
    std::size_t clause_unused_conflicts(ClauseId id) const;
    void set_clause_unused_conflicts(ClauseId id, std::size_t num_conflicts);

    const ClauseTierConfig& tier_config() const noexcept { return tier_config_; }

    /// @pre No clause has been added yet, and `config.core_lbd <= config.tier2_lbd`.
    void set_tier_config(const ClauseTierConfig& config);

    /// @brief The tier a learned clause of this LBD enters.
    ClauseTier tier_for_lbd(std::size_t lbd) const;

    /// @brief The next clause to try, or `std::nullopt` once propagation is quiescent.
    virtual std::optional<ClauseId> next() = 0;

    virtual std::optional<ClauseSelection> next_with_hint();

    virtual void add_starting_clause(Clause clause); ///< Takes the next id.

    /// @brief Add a learned asserting clause under the next id; `trail` already reflects
    /// the backjump.
    virtual void add_asserting_clause(Clause clause, const Trail& trail,
                                      const AssertingClauseInfo& info);

    /// @brief Note an equation learned by propagation, already reduced by the span it was
    /// added to and already on `trail`.
    virtual void update_with_propagation(const Equation& reduced_eqn, const Trail& trail);

    virtual void update_with_decision(const Equation& reduced_eqn, const Trail& trail);

    virtual void update_with_decision_clause(std::optional<ClauseId> clause_id);

    /// @brief Note a watch move, which the default ignores.
    void update_with_new_watch(ClauseId id, ClauseRowIndex old_watch, WatchType type,
                               const Trail& trail) override;

    /// @brief Note that every clause was rewritten into a compacted variable numbering.
    virtual void update_with_compaction(const CompactionTransform& transform, const Trail& trail);

protected:
    /// @brief Store a clause under the next id, with no other side effects.
    virtual void add_clause(Clause clause);

    /// @brief React to a learned equation, propagated or decided alike.
    virtual void update_with_new_equation(const Equation& reduced_eqn, const Trail& trail);

#ifndef NDEBUG
    void debug_assert_database_synchronized() const;
    void debug_assert_clause_id(ClauseId id) const;
#endif

    ClauseTierConfig tier_config_;

    std::vector<Clause> clauses_; ///< A clause's id is its index.
    std::vector<std::uint8_t> clause_active_;
    std::vector<std::uint8_t> clause_learned_;
    std::vector<std::uint8_t> clause_used_since_deletion_;
    std::vector<ClauseTier> clause_tier_;
    std::vector<std::size_t> clause_lbd_;
    std::vector<std::size_t> clause_unused_conflicts_;
};

} // namespace xorcle
