#include "heuristics/bve/bve.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <numeric>
#include <optional>
#include <span>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "algebra/equation.h"
#include "algebra/equation_template.h"
#include "algebra/gf2_vector.h"
#include "algebra/gf2_vector_of_repr.h"
#include "algebra/pivot_table.h"
#include "algebra/reduce_result.h"
#include "heuristics/bve/gate_solver.h"
#include "util/index_set.h"
#include "util/random.h"

namespace xorcle {

namespace {

using Var = std::uint32_t;
constexpr Var no_var = static_cast<Var>(-1);

/// @brief The largest cost, `clauses² × max(1, 2^(rank - 6))`, of a gate search done by its
/// truth table rather than the gate solver: 5e5, as any bound from 4e5 to 1e6 gives the gate
/// searches their least total time.
constexpr std::size_t gate_max_table_cost = 500'000;

/// @brief The largest share of the present clauses a try may touch and still search for a
/// gate: 10%.
constexpr double gate_max_clause_share = 0.1;

/// @brief Index of a direction: an equation without its constant.
using DirectionId = std::uint32_t;

template <GF2Vector V>
std::size_t constant_bit(const V& row) {
    return row.num_bits() - 1;
}

template <GF2Vector V>
bool constant_of(const V& row) {
    return row.get_bit(constant_bit(row));
}

/// @brief Add `0 = 1` to @p row.
template <GF2Vector V>
void negate(V& row) {
    row.flip_bit(constant_bit(row));
}

template <GF2Vector V>
std::size_t num_vars_of(const V& row) {
    return row.num_set_bits() - (constant_of(row) ? 1U : 0U);
}

/// @brief The lowest set bit of @p vector that is at least @p from; `no_var` if none.
template <GF2Vector V>
Var lowest_bit_from(const V& vector, std::size_t from) {
    std::optional<std::size_t> bit;
    if (from == 0) {
        bit = vector.leading_bit();
    } else if (from <= vector.num_bits()) {
        bit = vector.next_bit(from - 1);
    }
    return bit.has_value() ? static_cast<Var>(*bit) : no_var;
}

/// @brief The lowest variable of @p row that is at least @p from; `no_var` if none.
template <GF2Vector V>
Var lowest_var_from(const V& row, Var from) {
    const Var bit = lowest_bit_from(row, from);
    return bit != no_var && bit == constant_bit(row) ? no_var : bit;
}

template <GF2Vector V>
Var lowest_var(const V& row) {
    const Var var = lowest_var_from(row, 0);
    assert(var != no_var && "the equation mentions no variable");
    return var;
}

template <GF2Vector V, class F>
void for_each_var(const V& row, F&& f) {
    const std::size_t constant = constant_bit(row);
    row.for_each_set_bit([&](const std::size_t bit) {
        if (bit != constant) {
            f(static_cast<Var>(bit));
        }
    });
}

/// @brief A row's part of a hash of rows: @p direction_hash, its `hash_except_last_bit()`,
/// with its constant.
template <GF2Vector V>
std::size_t row_hash(const V& row, const std::size_t direction_hash) {
    return direction_hash ^ (constant_of(row) ? 1U : 0U);
}

/// @brief A hash of rows in order, given the hash of the rows before and the next row's part.
constexpr std::size_t hash_with_row(const std::size_t hash, const std::size_t row_hash) {
    return (std::rotl(hash, 31) ^ row_hash) * 0x9e3779b97f4a7c15U;
}

template <GF2Vector V>
VarListEquation var_list_of(const V& row) {
    VarListEquation out;
    out.constant = constant_of(row);
    for_each_var(row, [&out](const Var var) { out.vars.push_back(var); });
    return out;
}

template <GF2Vector V>
std::size_t hash_of(const std::vector<V>& rows) {
    std::size_t hash = 0;
    for (const V& row : rows) {
        hash = hash_with_row(hash, row_hash(row, row.hash_except_last_bit()));
    }
    return hash;
}

template <GF2Vector V>
std::size_t equation_bits(const std::vector<V>& rows) {
    std::size_t bits = 0;
    for (const V& row : rows) {
        bits += num_vars_of(row);
    }
    return bits;
}


/// @brief Workspace of `canonical_in_place()`.
struct CanonicalWork {
    std::vector<std::uint32_t> basis; ///< The rows leading a variable, in the order found.
    std::vector<Var> lead_of;
    /// @brief On return, the rows of the reduced row-echelon form in rising order of their leading variables.
    std::vector<std::uint32_t> by_lead;
};

/// @brief Reduce @p rows in place to their span's reduced row-echelon form, each row led by its lowest
/// variable, and name its rows in `work.by_lead`; false if the span holds `0 = 1`.
/// @pre @p leads is empty, and is left empty.
template <GF2Vector V>
bool canonical_in_place(std::span<V> rows, PivotTable& leads, CanonicalWork& work) {
    work.basis.clear();
    work.lead_of.clear();
    bool holds_eq_false = false;
    // A row of the basis mentions no variable leading an earlier row.
    for (std::size_t i = 0; i < rows.size(); ++i) {
        V& row = rows[i];
        leads.reduce(row, 0, [&](const std::size_t basis_idx) { row.plus_assign(rows[work.basis[basis_idx]]); });
        const ReduceResult reduced = classify(row);
        if (reduced.kind == ReduceResult::Kind::eq_true) {
            continue;
        }
        if (reduced.kind == ReduceResult::Kind::eq_false) {
            holds_eq_false = true;
            break;
        }
        const auto lead = static_cast<Var>(reduced.bit);
        leads.set(lead, work.basis.size());
        work.lead_of.push_back(lead);
        work.basis.push_back(static_cast<std::uint32_t>(i));
    }
    std::vector<std::uint32_t>& by_lead = work.by_lead;
    by_lead.resize(work.basis.size());
    std::iota(by_lead.begin(), by_lead.end(), 0U);
    std::sort(by_lead.begin(), by_lead.end(),
              [&work](std::uint32_t a, std::uint32_t b) { return work.lead_of[a] < work.lead_of[b]; });
    if (!holds_eq_false) {
        // From the highest leading variable down, so that the rows added are reduced already.
        for (auto i = by_lead.rbegin(); i != by_lead.rend(); ++i) {
            V& row = rows[work.basis[*i]];
            leads.reduce(row, work.lead_of[*i] + 1,
                         [&](const std::size_t basis_idx) { row.plus_assign(rows[work.basis[basis_idx]]); });
        }
    }
    for (const Var lead : work.lead_of) {
        leads.reset(lead);
    }
    if (holds_eq_false) {
        return false;
    }
    for (std::uint32_t& idx : by_lead) {
        idx = work.basis[idx];
    }
    return true;
}

/// @brief The span of @p spanning in reduced row-echelon form, each row led by
/// its lowest variable, in rising order of those; nothing if the span holds `0 = 1`.
/// @pre @p leads is empty, and is left empty.
template <GF2Vector V>
std::optional<std::vector<V>> canonical(std::vector<V> spanning, PivotTable& leads) {
    CanonicalWork work;
    if (!canonical_in_place(std::span<V>(spanning), leads, work)) {
        return std::nullopt;
    }
    std::vector<V> in_order;
    in_order.reserve(work.by_lead.size());
    for (const std::uint32_t row_idx : work.by_lead) {
        in_order.push_back(std::move(spanning[row_idx]));
    }
    return in_order;
}

/// @brief A clause a flip set touches: one row it changes, and a basis of the
/// rest of the linear negation that it does not.
template <GF2Vector V>
struct SplitClause {
    explicit SplitClause(std::size_t num_bits) : changed(num_bits) {}

    V changed;
    std::vector<V> rest; ///< Its first `num_rest`.
    std::size_t num_rest = 0;
    std::span<const V> rests() const { return {rest.data(), num_rest}; }
};

bool odd(std::uint32_t mask) { return (std::popcount(mask) & 1) != 0; }

void plus_or(std::vector<std::uint64_t>& bits, const std::vector<std::uint64_t>& other) {
    for (std::size_t word_idx = 0; word_idx < bits.size(); ++word_idx) {
        bits[word_idx] |= other[word_idx];
    }
}

/// @brief The clauses of a formula in canonical form, and of a pass its kept rows.
///
/// A sum of a pass's kept rows is a vector over the kept rows, of `num_vars` bits
/// as the kept rows are independent.
template <GF2Vector V>
class Formula {
public:
    using Rows = std::vector<V>;

    struct StoredClause {
        Rows rows; ///< In rising order of their lowest variables.
        std::size_t hash = 0; ///< Of `rows`, by `hash_of()`.
        std::vector<DirectionId> row_directions;
        ClauseId input_id = 0;
        std::array<std::uint32_t, 2> parent_clause_idxs{}; ///< Of a resolvent.
        /// @brief Once removed, the elimination that removed it and its place
        /// among that elimination's removed clauses.
        std::size_t elimination_idx = 0;
        std::size_t removed_clause_idx = 0;
        /// @brief Of a root equation, its derivation among `derivations()`.
        std::optional<std::size_t> derivation_idx;
        bool present = true;
    };

    /// @brief A clause a derivation's line cites: a clause by its index, or a lemma by its place.
    struct Cited {
        bool is_lemma;
        std::uint32_t idx;
    };

    /// @brief The lines that derive a root equation: lemmas, each in its linear
    /// negation with the clauses it cites, then the equation's own citations.
    struct Derivation {
        struct Lemma {
            Rows rows;
            std::vector<Cited> reasons;
        };
        std::vector<Lemma> lemmas;
        std::vector<Cited> reasons;
    };

    Formula(std::size_t num_vars, const BveConfig& config, bool with_proof, Rng& rng)
        : num_vars_(num_vars),
          config_(config),
          with_proof_(with_proof),
          rng_(rng),
          refutation_rng_(rng()),
          leads_(num_vars + 1),
          direction_(num_vars + 1),
          echelon_leads_(num_vars + 1),
          reduced_(num_vars + 1),
          held_pivots_(num_vars),
          beyond_held_(num_vars) {}

    const std::vector<StoredClause>& clauses() const noexcept { return clauses_; }
    const std::vector<Elimination>& eliminations() const noexcept { return eliminations_; }
    bool found_empty_clause() const noexcept { return found_empty_clause_; }
    std::size_t num_eliminated_by_gate() const noexcept { return num_eliminated_by_gate_; }
    std::size_t num_root_equations() const noexcept { return num_root_equations_; }
    double gate_time_s() const noexcept { return gate_time_s_; }
    const std::vector<Derivation>& derivations() const noexcept { return derivations_; }

    /// @brief The canonical form of the clause with linear negation spanned by
    /// @p spanning; nothing if the span holds `0 = 1`.
    std::optional<Rows> canonical_of(Rows spanning) {
        return canonical(std::move(spanning), leads_);
    }

    /// @pre `rows` is canonical, and no clause added has its span.
    void add_input_clause(Rows rows, ClauseId id) {
        assert(!is_present(rows) && "two input clauses have one span");
        const std::uint32_t clause_idx = add(std::move(rows), {});
        clauses_[clause_idx].input_id = id;
    }

    /// @brief One pass: span the clauses in a uniformly random order, split
    /// the span by the walk back from the order's end and eliminate in each group.
    void run_pass() {
        ++pass_id_;
        order_.clear();
        for (std::uint32_t clause_idx = 0; clause_idx < clauses_.size(); ++clause_idx) {
            if (clauses_[clause_idx].present) {
                order_.push_back(clause_idx);
            }
        }
        if (order_.empty()) {
            return;
        }
        shuffle_uniformly(order_, rng_);
        span_in_order();
        in_pass_ = true;
        has_pass_flip_sets_ = false;

        walk_back();
        eliminate_in_groups();

        in_pass_ = false;
        clear_echelon();
    }

    /// @brief The variables some present clause mentions, over `num_vars + 1` columns.
    IndexSet mentioned_vars() const {
        IndexSet mentioned(num_vars_ + 1);
        for (const StoredClause& clause : clauses_) {
            if (!clause.present) {
                continue;
            }
            for (const V& row : clause.rows) {
                for_each_var(row, [&mentioned](const Var var) { mentioned.set(var); });
            }
        }
        return mentioned;
    }

private:
    struct Direction {
        std::optional<V> vars; ///< With no constant; nothing while the id is free.
        std::size_t hash = 0; ///< Of `vars`, by `hash_except_last_bit()`.
        std::uint32_t num_rows = 0; ///< The clause rows with this direction.
    };

    // --- The clauses -----------------------------------------------------------

    /// @return The clause's index.
    std::uint32_t add(Rows rows, std::array<std::uint32_t, 2> parent_clause_idxs) {
        const std::uint32_t clause_idx = static_cast<std::uint32_t>(clauses_.size());
        assert(!is_present(rows) && "a clause added has the span of a present one");
        StoredClause clause;
        clause.rows = std::move(rows);
        clause.parent_clause_idxs = parent_clause_idxs;
        const std::size_t width = clause.rows.size();
        clause.row_directions.reserve(width);
        for (std::size_t row_idx = 0; row_idx < width; ++row_idx) {
            const V& row = clause.rows[row_idx];
            const std::size_t direction_hash = row.hash_except_last_bit();
            clause.hash = hash_with_row(clause.hash, row_hash(row, direction_hash));
            const DirectionId direction = intern(row, direction_hash);
            clause.row_directions.push_back(direction);
            hold_row(direction);
        }
        present_clause_idxs_by_hash_.emplace(clause.hash, clause_idx);
        clauses_.push_back(std::move(clause));
        if (in_pass_) {
            register_users(clause_idx);
        }
        return clause_idx;
    }

    Rows remove(std::uint32_t clause_idx) {
        StoredClause& clause = clauses_[clause_idx];
        assert(clause.present && "the clause was removed already");
        clause.present = false;
        erase_entry(present_clause_idxs_by_hash_, clause.hash, clause_idx);
        for (const DirectionId direction : clause.row_directions) {
            assert(directions_[direction].num_rows != 0 && "a row was released more often than held");
            --directions_[direction].num_rows;
        }
        for (const DirectionId direction : clause.row_directions) {
            forget_if_unused(direction);
        }
        clause.row_directions.clear();
        return std::move(clause.rows);
    }

    /// @brief Whether a present clause has the rows `row_at(0)`, ..., `row_at(num_rows - 1)`,
    /// in that order, whose hash by `hash_of()` is @p hash.
    template <class RowAt>
    bool is_present(const std::size_t hash, const std::size_t num_rows, RowAt row_at) const {
        const auto [begin, end] = present_clause_idxs_by_hash_.equal_range(hash);
        return std::any_of(begin, end, [&](const auto& entry) {
            return rows_equal(clauses_[entry.second].rows, num_rows, row_at);
        });
    }

    bool is_present(const Rows& rows) const {
        return is_present(hash_of(rows), rows.size(),
                          [&rows](const std::size_t row_idx) -> const V& { return rows[row_idx]; });
    }

    template <class RowAt>
    static bool rows_equal(const Rows& rows, const std::size_t num_rows, RowAt row_at) {
        if (rows.size() != num_rows) {
            return false;
        }
        for (std::size_t row_idx = 0; row_idx < num_rows; ++row_idx) {
            if (!rows[row_idx].equals(row_at(row_idx))) {
                return false;
            }
        }
        return true;
    }

    /// @brief Erase the entry @p hash to @p idx, which @p table holds.
    template <class Table>
    static void erase_entry(Table& table, const std::size_t hash,
                            const Table::mapped_type idx) {
        const auto [begin, end] = table.equal_range(hash);
        const auto found =
            std::find_if(begin, end, [idx](const auto& entry) { return entry.second == idx; });
        assert(found != end && "the entry is not in the table");
        table.erase(found);
    }

    // --- The directions ------------------------------------------------------------

    /// @brief The id of the direction of @p row, whose `hash_except_last_bit()` is @p hash,
    /// new if no clause has it yet.
    DirectionId intern(const V& row, const std::size_t hash) {
        const auto [begin, end] = direction_ids_by_hash_.equal_range(hash);
        const auto found = std::find_if(begin, end, [&](const auto& entry) {
            return directions_[entry.second].vars->equals_except_last_bit(row);
        });
        if (found != end) {
            return found->second;
        }
        DirectionId direction;
        if (!free_directions_.empty()) {
            direction = free_directions_.back();
            free_directions_.pop_back();
        } else {
            direction = static_cast<DirectionId>(directions_.size());
            directions_.emplace_back();
            pass_sum_.emplace_back(num_vars_);
            pass_of_.push_back(0);
            label_.push_back(0);
            label_at_.push_back(0);
        }
        V& vars = directions_[direction].vars.emplace(row);
        vars.reset_bit(constant_bit(vars));
        directions_[direction].hash = hash;
        direction_ids_by_hash_.emplace(hash, direction);
        return direction;
    }

    /// @brief Count a clause row with this direction; in a pass, a row new
    /// to it is a sum of rows read, whose kept rows are found by reduction.
    void hold_row(DirectionId direction) {
        Direction& entry = directions_[direction];
        if (entry.num_rows++ == 0 && in_pass_ && pass_of_[direction] != pass_id_) {
            reduced_.assign_from(*entry.vars);
            reduce_in_pass(reduced_, pass_sum_[direction]);
            assert(lowest_var_from(reduced_, 0) == no_var && "a new row is not in the span read");
            pass_of_[direction] = pass_id_;
        }
    }

    /// @brief Let go of a direction no clause row has.
    void forget_if_unused(DirectionId direction) {
        Direction& entry = directions_[direction];
        if (entry.num_rows != 0 || !entry.vars.has_value()) {
            return;
        }
        erase_entry(direction_ids_by_hash_, entry.hash, direction);
        entry.vars.reset();
        pass_of_[direction] = 0;
        free_directions_.push_back(direction);
    }

    /// @brief For each of the @p num_kept kept rows of the echelon, a flip set
    /// that changes it and no other; leaves the echelon reduced.
    void find_flip_sets_of(std::size_t num_kept) {
        // To reduced form: then the leading variables of the echelon rows that
        // sum a kept row are such a flip set.
        std::vector<std::uint32_t> by_lead(echelon_rows_.size());
        std::iota(by_lead.begin(), by_lead.end(), 0U);
        std::sort(by_lead.begin(), by_lead.end(), [this](std::uint32_t a, std::uint32_t b) {
            return echelon_lead_of_[a] > echelon_lead_of_[b];
        });
        for (const std::uint32_t i : by_lead) {
            echelon_leads_.reduce(echelon_rows_[i], echelon_lead_of_[i] + 1, [&](const std::size_t echelon_idx) {
                echelon_rows_[i].plus_assign(echelon_rows_[echelon_idx]);
                pass_sums_[i].plus_assign(pass_sums_[echelon_idx]);
            });
        }
        std::vector<std::vector<std::size_t>> bits_of(num_kept);
        // In rising order of the leading variables, so that each list is ascending.
        for (auto i = by_lead.rbegin(); i != by_lead.rend(); ++i) {
            pass_sums_[*i].for_each_set_bit(
                [&](const std::size_t kept_idx) { bits_of[kept_idx].push_back(echelon_lead_of_[*i]); });
        }
        flip_set_of_kept_.clear();
        for (std::size_t kept_idx = 0; kept_idx < num_kept; ++kept_idx) {
            flip_set_of_kept_.emplace_back(num_vars_).assign_bits(bits_of[kept_idx]);
        }
    }

    void clear_echelon() {
        for (const Var lead : echelon_lead_of_) {
            echelon_leads_.reset(lead);
        }
        echelon_rows_ = {};
        pass_sums_ = {};
        echelon_lead_of_ = {};
    }

    // --- A pass -------------------------------------------------------------------

    /// @brief Reduce each clause's rows, in `order_`, against the span of the
    /// rows kept so far, keeping each that is not in it, and record for each row
    /// the kept rows it sums and for each kept row the clauses using it.
    void span_in_order() {
        adder_of_kept_.clear();
        added_begin_.clear();
        for (const std::uint32_t clause_idx : order_) {
            added_begin_.push_back(static_cast<std::uint32_t>(adder_of_kept_.size()));
            for (const DirectionId direction : clauses_[clause_idx].row_directions) {
                if (pass_of_[direction] == pass_id_) {
                    continue;
                }
                pass_of_[direction] = pass_id_;
                V& sum_of_row = pass_sum_[direction];
                V rest = *directions_[direction].vars;
                reduce_in_pass(rest, sum_of_row);
                const Var lead = lowest_var_from(rest, 0);
                if (lead == no_var) {
                    continue;
                }
                const auto kept_idx = static_cast<std::uint32_t>(adder_of_kept_.size());
                adder_of_kept_.push_back(clause_idx);
                // The row is the part reduced away plus itself.
                sum_of_row.set_bit(kept_idx);
                pass_sums_.push_back(sum_of_row);
                sum_of_row.set_zero();
                sum_of_row.set_bit(kept_idx);
                echelon_leads_.set(lead, echelon_rows_.size());
                echelon_lead_of_.push_back(lead);
                echelon_rows_.push_back(std::move(rest));
            }
        }
        added_begin_.push_back(static_cast<std::uint32_t>(adder_of_kept_.size()));
        const std::size_t num_kept = adder_of_kept_.size();
        users_.assign(num_kept, {});
        last_user_.assign(num_kept, none);
        unknown_idx_of_kept_.assign(num_kept, none);
        for (const std::uint32_t clause_idx : order_) {
            register_users(clause_idx);
        }
    }

    /// @brief Reduce @p rest against the pass's echelon, setting @p sum_of_row
    /// to the kept rows of the part reduced away.
    void reduce_in_pass(V& rest, V& sum_of_row) {
        sum_of_row.set_zero();
        echelon_leads_.reduce(rest, 0, [&](const std::size_t echelon_idx) {
            rest.plus_assign(echelon_rows_[echelon_idx]);
            sum_of_row.plus_assign(pass_sums_[echelon_idx]);
        });
    }

    /// @brief Put clause @p clause_idx on the list of each kept row its rows sum.
    void register_users(std::uint32_t clause_idx) {
        for (const DirectionId direction : clauses_[clause_idx].row_directions) {
            pass_sum_[direction].for_each_set_bit([&](const std::size_t kept_idx) {
                if (last_user_[kept_idx] != clause_idx) {
                    last_user_[kept_idx] = clause_idx;
                    users_[kept_idx].push_back(clause_idx);
                }
            });
        }
    }

    std::uint32_t find_group(std::uint32_t group) {
        while (group_parent_[group] != group) {
            group_parent_[group] = group_parent_[group_parent_[group]];
            group = group_parent_[group];
        }
        return group;
    }

    bool has_held_pivot(const V& sum) const { return sum.first_bit_in(held_pivots_, 0).has_value(); }

    /// @brief The held pivot of @p sum at or after kept row @p from; `none` if none.
    std::uint32_t held_pivot_from(const V& sum, std::size_t from) const {
        const std::optional<std::size_t> kept_idx = sum.first_bit_in(held_pivots_, from);
        return kept_idx.has_value() ? static_cast<std::uint32_t>(*kept_idx) : none;
    }

    /// @brief The kept rows of @p sum less the rows of the clauses held in the
    /// core subspace, in a kept buffer valid until the next call: @p sum plus the held
    /// vector of each held pivot it has, which leaves no held pivot.
    const V& beyond_held_in_place(const V& sum) {
        V& rest = beyond_held_;
        rest.assign_from(sum);
        for (std::uint32_t kept_idx = held_pivot_from(sum, 0); kept_idx != none;
             kept_idx = held_pivot_from(sum, kept_idx + 1)) {
            rest.plus_assign(held_[held_of_pivot_[kept_idx]]);
        }
        return rest;
    }

    /// @brief Add the row that sums @p sum to the span of the clauses held in
    /// the core subspace, kept in reduced form: each held vector led by its highest
    /// kept row, which no other has.
    void hold_in_core(const V& sum) {
        V vector = beyond_held_in_place(sum);
        if (!vector.leading_bit().has_value()) {
            return;
        }
        std::uint32_t pivot = 0;
        vector.for_each_set_bit([&pivot](const std::size_t kept_idx) { pivot = static_cast<std::uint32_t>(kept_idx); });
        const auto add_to_held_with = [&](const std::uint32_t held_idx) {
            vector.for_each_set_bit([&](const std::size_t kept_idx) {
                if (kept_idx != pivot) {
                    held_with_[kept_idx].push_back(held_idx);
                }
            });
        };
        for (const std::uint32_t held_idx : held_with_[pivot]) {
            V& other = held_[held_idx];
            if (other.get_bit(pivot)) {
                other.plus_assign(vector);
                add_to_held_with(held_idx);
            }
        }
        held_with_[pivot].clear();
        const auto held_idx = static_cast<std::uint32_t>(held_.size());
        add_to_held_with(held_idx);
        held_of_pivot_[pivot] = held_idx;
        held_pivots_.set(pivot);
        held_pivot_.push_back(pivot);
        held_.push_back(std::move(vector));
    }

    /// @brief Call @p f with each clause whose rows may sum kept row @p kept_idx
    /// less the held rows: those using it or a held pivot whose vector has it.
    template <class F>
    void for_each_user_beyond_held(std::uint32_t kept_idx, F&& f) {
        for (const std::uint32_t clause_idx : users_[kept_idx]) {
            f(clause_idx);
        }
        for (const std::uint32_t held_idx : held_with_[kept_idx]) {
            if (held_[held_idx].get_bit(kept_idx)) {
                const std::uint32_t pivot = held_pivot_[held_idx];
                for (const std::uint32_t clause_idx : users_[pivot]) {
                    f(clause_idx);
                }
            }
        }
    }

    /// @brief Split the span into the core subspace and groups by moving a cut
    /// back from the end of `order_`.
    ///
    /// The rows a clause past the cut kept, less the span of the clauses held in
    /// the core subspace, become group rows, joined with the groups of the
    /// clauses whose rows sum them less that span; unless the group would
    /// hold more than the cap, when the clause is held in the core subspace instead.
    void walk_back() {
        const std::size_t num_kept = adder_of_kept_.size();
        group_of_kept_.assign(num_kept, none);
        group_of_clause_.assign(clauses_.size(), none);
        group_parent_.clear();
        group_size_.clear();
        group_mark_.clear();
        held_.clear();
        held_pivot_.clear();
        held_pivots_.clear();
        held_of_pivot_.assign(num_kept, none);
        held_with_.assign(num_kept, {});
        std::vector<std::uint32_t> joined;
        std::vector<std::uint32_t> own_group_rows;
        std::vector<std::uint32_t> affected;
        for (std::size_t pos = order_.size(); pos-- > 0;) {
            const std::uint32_t begin = added_begin_[pos];
            const std::uint32_t end = added_begin_[pos + 1];
            own_group_rows.clear();
            for (std::uint32_t kept_idx = begin; kept_idx < end; ++kept_idx) {
                if (held_of_pivot_[kept_idx] == none) {
                    own_group_rows.push_back(kept_idx);
                }
            }
            if (own_group_rows.empty()) {
                continue;
            }
            const auto is_group_row = [&](std::uint32_t kept_idx) {
                return kept_idx >= begin && kept_idx < end && held_of_pivot_[kept_idx] == none;
            };
            // Whether a row's kept rows less the held span have a group row: the
            // parity of each group row over the row's kept rows and the held
            // vectors of its held pivots, read only within the clause's kept rows.
            // Calls f with each kept row of `vector` among the clause's own, [begin, end).
            const auto for_each_in_clause = [&](const V& vector, auto&& f) {
                for (Var kept_idx = lowest_bit_from(vector, begin); kept_idx != no_var && kept_idx < end;
                     kept_idx = lowest_bit_from(vector, kept_idx + 1)) {
                    f(kept_idx);
                }
            };
            const auto sums_group_row = [&](std::uint32_t clause_idx) {
                return std::ranges::any_of(clauses_[clause_idx].row_directions, [&](DirectionId direction) {
                    const V& sum = pass_sum_[direction];
                    if (!has_held_pivot(sum)) {
                        bool found = false;
                        for_each_in_clause(sum, [&](const Var kept_idx) { found = found || is_group_row(kept_idx); });
                        return found;
                    }
                    group_row_parity_.assign(end - begin, 0);
                    for_each_in_clause(sum, [&](const Var kept_idx) { group_row_parity_[kept_idx - begin] ^= 1U; });
                    for (std::uint32_t kept_idx = held_pivot_from(sum, 0); kept_idx != none;
                         kept_idx = held_pivot_from(sum, kept_idx + 1)) {
                        const V& held = held_[held_of_pivot_[kept_idx]];
                        for_each_in_clause(held, [&](const Var in_clause) { group_row_parity_[in_clause - begin] ^= 1U; });
                    }
                    for (std::uint32_t kept_idx = begin; kept_idx < end; ++kept_idx) {
                        if (group_row_parity_[kept_idx - begin] != 0 && is_group_row(kept_idx)) {
                            return true;
                        }
                    }
                    return false;
                });
            };
            // The size first, from the clauses in groups only: most clauses
            // the cap holds are decided without reading every user.
            std::size_t size = own_group_rows.size();
            bool is_over = size > config_.group_cap;
            joined.clear();
            ++mark_stamp_;
            for (const std::uint32_t kept_idx : own_group_rows) {
                if (is_over) {
                    break;
                }
                for_each_user_beyond_held(kept_idx, [&](std::uint32_t clause_idx) {
                    if (is_over || group_of_clause_[clause_idx] == none || !clauses_[clause_idx].present ||
                        clause_mark(clause_idx) == mark_stamp_) {
                        return;
                    }
                    clause_mark(clause_idx) = mark_stamp_;
                    const std::uint32_t group = find_group(group_of_clause_[clause_idx]);
                    if (group_mark_[group] == mark_stamp_ || !sums_group_row(clause_idx)) {
                        return;
                    }
                    group_mark_[group] = mark_stamp_;
                    joined.push_back(group);
                    size += group_size_[group];
                    is_over = size > config_.group_cap;
                });
            }
            affected.clear();
            if (!is_over) {
                ++mark_stamp_;
                for (const std::uint32_t kept_idx : own_group_rows) {
                    for_each_user_beyond_held(kept_idx, [&](std::uint32_t clause_idx) {
                        if (clauses_[clause_idx].present && clause_mark(clause_idx) != mark_stamp_) {
                            clause_mark(clause_idx) = mark_stamp_;
                            if (sums_group_row(clause_idx)) {
                                affected.push_back(clause_idx);
                            }
                        }
                    });
                }
            }
            if (is_over) {
                for (const DirectionId direction : clauses_[order_[pos]].row_directions) {
                    hold_in_core(pass_sum_[direction]);
                }
                continue;
            }
            const auto group = static_cast<std::uint32_t>(group_parent_.size());
            group_parent_.push_back(group);
            group_size_.push_back(static_cast<std::uint32_t>(size));
            group_mark_.push_back(0);
            for (const std::uint32_t other : joined) {
                group_parent_[other] = group;
            }
            for (const std::uint32_t kept_idx : own_group_rows) {
                group_of_kept_[kept_idx] = group;
            }
            for (const std::uint32_t clause_idx : affected) {
                group_of_clause_[clause_idx] = group;
            }
        }
    }

    /// @brief The groups' rows, the group first reached from the order's end first.
    void eliminate_in_groups() {
        std::vector<std::uint32_t> index_of_group(group_parent_.size(), none);
        std::vector<std::vector<std::uint32_t>> groups;
        for (std::size_t pos = order_.size(); pos-- > 0;) {
            for (std::uint32_t kept_idx = added_begin_[pos]; kept_idx < added_begin_[pos + 1]; ++kept_idx) {
                if (group_of_kept_[kept_idx] == none) {
                    continue;
                }
                const std::uint32_t group = find_group(group_of_kept_[kept_idx]);
                if (index_of_group[group] == none) {
                    index_of_group[group] = static_cast<std::uint32_t>(groups.size());
                    groups.emplace_back();
                }
                groups[index_of_group[group]].push_back(kept_idx);
            }
        }
        for (const std::vector<std::uint32_t>& group_rows : groups) {
            if (found_empty_clause_) {
                return;
            }
            eliminate_in_group(group_rows);
        }
    }

    /// @brief The group rows of the group in hand that @p direction sums less the
    /// held rows, as bits over the group's rows; computed once per `label_stamp_`.
    std::uint32_t label_of(DirectionId direction) {
        if (label_at_[direction] != label_stamp_) {
            label_at_[direction] = label_stamp_;
            std::uint32_t label = 0;
            beyond_held_in_place(pass_sum_[direction]).for_each_set_bit([&](const std::size_t kept_idx) {
                if (unknown_idx_of_kept_[kept_idx] != none) {
                    label |= std::uint32_t{1} << unknown_idx_of_kept_[kept_idx];
                }
            });
            label_[direction] = label;
        }
        return label_[direction];
    }

    /// @brief Try the hyperplanes of one group, fewest clauses touched first and
    /// equals drawn uniformly, one hyperplane per coset of those accepted, until
    /// none is left; a hyperplane tried is tried again only once the clauses it
    /// touches have changed.
    void eliminate_in_group(const std::vector<std::uint32_t>& group_rows) {
        const std::size_t size = group_rows.size();
        for (std::size_t i = 0; i < size; ++i) {
            unknown_idx_of_kept_[group_rows[i]] = static_cast<std::uint32_t>(i);
        }
        const std::uint32_t num_hyperplanes = std::uint32_t{1} << size;
        const std::size_t num_words = (num_hyperplanes + 63) / 64;
        // Per label, the hyperplanes sharing an odd number of group rows with it.
        std::unordered_map<std::uint32_t, std::vector<std::uint64_t>> hyperplanes_of_label;
        const auto hyperplanes_touching = [&](std::uint32_t label) -> const std::vector<std::uint64_t>& {
            auto [found, is_new] = hyperplanes_of_label.try_emplace(label);
            if (is_new) {
                found->second.assign(num_words, 0);
                for (std::uint32_t hyperplane = 1; hyperplane < num_hyperplanes; ++hyperplane) {
                    if (odd(label & hyperplane)) {
                        found->second[hyperplane >> 6] |= std::uint64_t{1} << (hyperplane & 63);
                    }
                }
            }
            return found->second;
        };
        // Per clause touched by some hyperplane, the hyperplanes touching it.
        std::unordered_map<std::uint32_t, std::vector<std::uint64_t>> leftovers;
        std::vector<std::uint32_t> clauses_touched(num_hyperplanes, 0);
        std::vector<std::uint8_t> is_open(num_hyperplanes, 1);
        const auto count = [&](const std::vector<std::uint64_t>& hyperplanes_touching_clause, bool entering) {
            for (std::size_t word_idx = 0; word_idx < num_words; ++word_idx) {
                for (std::uint64_t word = hyperplanes_touching_clause[word_idx]; word != 0; word &= word - 1) {
                    const std::size_t hyperplane = (word_idx << 6) + static_cast<std::size_t>(std::countr_zero(word));
                    clauses_touched[hyperplane] = entering ? clauses_touched[hyperplane] + 1 : clauses_touched[hyperplane] - 1;
                    is_open[hyperplane] = 1;
                }
            }
        };
        const auto enter = [&](std::uint32_t clause_idx) {
            std::vector<std::uint64_t> hyperplanes_touching_clause(num_words, 0);
            for (const DirectionId direction : clauses_[clause_idx].row_directions) {
                const std::uint32_t label = label_of(direction);
                if (label != 0) {
                    plus_or(hyperplanes_touching_clause, hyperplanes_touching(label));
                }
            }
            if (std::ranges::any_of(hyperplanes_touching_clause, [](std::uint64_t word) { return word != 0; })) {
                count(hyperplanes_touching_clause, true);
                leftovers.emplace(clause_idx, std::move(hyperplanes_touching_clause));
            }
        };
        ++label_stamp_;
        ++mark_stamp_;
        for (const std::uint32_t kept_idx : group_rows) {
            for_each_user_beyond_held(kept_idx, [&](std::uint32_t clause_idx) {
                if (clauses_[clause_idx].present && clause_mark(clause_idx) != mark_stamp_) {
                    clause_mark(clause_idx) = mark_stamp_;
                    enter(clause_idx);
                }
            });
        }
        // The hyperplanes accepted, in reduced echelon form, each led by its lowest bit.
        std::vector<std::uint32_t> accepted;
        std::uint32_t accepted_leads = 0;
        std::vector<std::uint32_t> hyperplanes;
        std::vector<std::uint32_t> touched;
        while (!found_empty_clause_) {
            // Until an elimination nothing changes: one order serves.
            hyperplanes.clear();
            for (std::uint32_t hyperplane = 1; hyperplane < num_hyperplanes; ++hyperplane) {
                if ((hyperplane & accepted_leads) == 0 && is_open[hyperplane] != 0 && clauses_touched[hyperplane] != 0) {
                    hyperplanes.push_back(hyperplane);
                }
            }
            shuffle_uniformly(hyperplanes, rng_);
            std::ranges::stable_sort(hyperplanes, {},
                                     [&clauses_touched](std::uint32_t hyperplane) { return clauses_touched[hyperplane]; });
            std::optional<std::uint32_t> taken;
            std::size_t clauses_before = 0;
            std::size_t num_refusals = 0;
            for (const std::uint32_t hyperplane : hyperplanes) {
                is_open[hyperplane] = 0;
                touched.clear();
                for (const auto& [clause_idx, hyperplanes_touching_clause] : leftovers) {
                    if (((hyperplanes_touching_clause[hyperplane >> 6] >> (hyperplane & 63)) & 1U) != 0) {
                        touched.push_back(clause_idx);
                    }
                }
                std::ranges::sort(touched);
                clauses_before = clauses_.size();
                if (try_hyperplane(hyperplane, group_rows, touched)) {
                    taken = hyperplane;
                    break;
                }
                // The root equations a refused try added.
                for (auto clause_idx = static_cast<std::uint32_t>(clauses_before); clause_idx < clauses_.size(); ++clause_idx) {
                    enter(clause_idx);
                }
                if (++num_refusals == config_.max_refusals_in_a_row) {
                    break;
                }
            }
            if (!taken.has_value()) {
                break;
            }
            const std::uint32_t hyperplane = *taken;
            const std::uint32_t lead = hyperplane & (~hyperplane + 1);
            for (std::uint32_t& row : accepted) {
                if ((row & lead) != 0) {
                    row ^= hyperplane;
                }
            }
            accepted.push_back(hyperplane);
            accepted_leads |= lead;
            // Ids of removed rows may be taken by new ones.
            ++label_stamp_;
            for (const std::uint32_t clause_idx : touched) {
                const auto found = leftovers.find(clause_idx);
                count(found->second, false);
                leftovers.erase(found);
            }
            for (auto clause_idx = static_cast<std::uint32_t>(clauses_before); clause_idx < clauses_.size(); ++clause_idx) {
                if (clauses_[clause_idx].present) {
                    enter(clause_idx);
                }
            }
        }
        for (const std::uint32_t kept_idx : group_rows) {
            unknown_idx_of_kept_[kept_idx] = none;
        }
    }

    /// @brief Eliminate by hyperplane @p hyperplane over @p group_rows,
    /// touching @p touched, the clauses with a row whose label shares an odd
    /// number of group rows with it.
    /// @return Whether it was accepted.
    bool try_hyperplane(std::uint32_t hyperplane, const std::vector<std::uint32_t>& group_rows,
                    const std::vector<std::uint32_t>& touched) {
        assert(!touched.empty() && "a hyperplane touches nothing");
        std::optional<Resolution> resolution =
            resolve(touched, [this, hyperplane](const StoredClause& clause, std::size_t row_idx) {
                return odd(label_of(clause.row_directions[row_idx]) & hyperplane);
            });
        if (!resolution.has_value()) {
            return false;
        }
        // The hyperplane holds the held rows: a held pivot changes with the group
        // rows of its vector the hyperplane changes.
        std::vector<std::uint32_t> changed_kept_idxs;
        std::unordered_map<std::uint32_t, std::uint8_t> held_parity;
        std::vector<std::uint32_t> holding;
        for (std::size_t i = 0; i < group_rows.size(); ++i) {
            if (((hyperplane >> i) & 1U) != 0) {
                changed_kept_idxs.push_back(group_rows[i]);
                // A held vector can be listed more than once.
                holding = held_with_[group_rows[i]];
                std::ranges::sort(holding);
                const auto repeated = std::ranges::unique(holding);
                holding.erase(repeated.begin(), repeated.end());
                for (const std::uint32_t held_idx : holding) {
                    if (held_[held_idx].get_bit(group_rows[i])) {
                        held_parity[held_idx] ^= 1U;
                    }
                }
            }
        }
        for (const auto& [held_idx, parity] : held_parity) {
            if (parity != 0) {
                changed_kept_idxs.push_back(held_pivot_[held_idx]);
            }
        }
        std::ranges::sort(changed_kept_idxs);
        replace(touched, std::move(*resolution), pass_flip_set_vars(changed_kept_idxs));
        return true;
    }

    /// @brief The variables of the flip set that changes exactly the kept rows
    /// @p changed_kept_idxs among the pass's kept rows, ascending.
    std::vector<std::uint32_t> pass_flip_set_vars(const std::vector<std::uint32_t>& changed_kept_idxs) {
        if (!has_pass_flip_sets_) {
            has_pass_flip_sets_ = true;
            find_flip_sets_of(adder_of_kept_.size());
        }
        V sum(num_vars_);
        for (const std::uint32_t kept_idx : changed_kept_idxs) {
            sum.plus_assign(flip_set_of_kept_[kept_idx]);
        }
        std::vector<std::uint32_t> vars;
        sum.for_each_set_bit([&vars](const std::size_t var) { vars.push_back(static_cast<std::uint32_t>(var)); });
        return vars;
    }

    std::size_t& clause_mark(std::uint32_t clause_idx) {
        if (clause_mark_.size() < clauses_.size()) {
            clause_mark_.resize(clauses_.size(), 0);
        }
        return clause_mark_[clause_idx];
    }

    /// @brief The resolvents an accepted elimination adds, in the order of
    /// their parents' places among the clauses resolved.
    struct Resolution {
        std::vector<Rows> resolvents;
        std::vector<std::array<std::uint32_t, 2>> parents;
        bool by_gate = false;
    };

    /// @brief Split each of @p clause_idxs by the rows @p changes says the flip
    /// set changes, and resolve the pairs, those with a gate clause only if a
    /// gate is found.
    /// @return Nothing if the resolvents have more rows in total than the clauses.
    /// @pre Each clause has a row the flip set changes.
    template <class Changes>
    std::optional<Resolution> resolve(const std::vector<std::uint32_t>& clause_idxs,
                                      Changes&& changes) {
        while (parts_.size() < clause_idxs.size()) {
            parts_.emplace_back(num_vars_ + 1);
        }
        const std::span<SplitClause<V>> parts(parts_.data(), clause_idxs.size());
        std::size_t rows_removed = 0;
        for (std::size_t part_idx = 0; part_idx < clause_idxs.size(); ++part_idx) {
            const StoredClause& clause = clauses_[clause_idxs[part_idx]];
            rows_removed += clause.rows.size();
            SplitClause<V>& part = parts[part_idx];
            part.num_rest = 0;
            bool found = false;
            for (std::size_t row_idx = 0; row_idx < clause.rows.size(); ++row_idx) {
                const V& row = clause.rows[row_idx];
                const bool is_changed = changes(clause, row_idx);
                if (is_changed && !found) {
                    part.changed.assign_from(row);
                    found = true;
                    continue;
                }
                if (part.rest.size() == part.num_rest) {
                    part.rest.emplace_back(num_vars_ + 1);
                }
                V& rest = part.rest[part.num_rest++];
                rest.assign_from(row);
                if (is_changed) {
                    rest.plus_assign(part.changed);
                }
            }
            assert(found && "the flip set does not touch the clause");
        }
        const std::optional<std::vector<std::uint8_t>> in_gate = find_gate(parts, clause_idxs);

        Resolution resolution;
        std::unordered_multimap<std::size_t, std::size_t> resolvent_idxs_by_hash;
        std::size_t rows_added = 0;
        // Whether the resolvents still have no more rows than the clauses removed.
        const auto resolve_pair = [&](const std::size_t i, const std::size_t j) {
            // The same row on both sides resolves to a tautology.
            if (parts[i].changed.equals(parts[j].changed)) {
                return true;
            }
            const std::size_t num_spanning = parts[i].num_rest + parts[j].num_rest + 1;
            while (spanning_.size() < num_spanning) {
                spanning_.emplace_back(num_vars_ + 1);
            }
            const std::span<V> spanning(spanning_.data(), num_spanning);
            std::size_t spanning_idx = 0;
            for (const std::size_t part_idx : {i, j}) {
                for (const V& row : parts[part_idx].rests()) {
                    spanning[spanning_idx++].assign_from(row);
                }
            }
            V& negated_changed = spanning[spanning_idx];
            negated_changed.assign_from(parts[i].changed);
            negated_changed.plus_assign(parts[j].changed);
            negate(negated_changed);
            if (!canonical_in_place(spanning, leads_, canonical_work_)) {
                return true;
            }
            const std::vector<std::uint32_t>& by_lead = canonical_work_.by_lead;
            const auto row_at = [&](const std::size_t row_idx) -> const V& {
                return spanning[by_lead[row_idx]];
            };
            std::size_t hash = 0;
            for (const std::uint32_t row_idx : by_lead) {
                const V& row = spanning[row_idx];
                hash = hash_with_row(hash, row_hash(row, row.hash_except_last_bit()));
            }
            if (is_present(hash, by_lead.size(), row_at)) {
                return true;
            }
            // By place among the clauses, and not yet by index.
            const std::array<std::uint32_t, 2> parents{static_cast<std::uint32_t>(i),
                                                       static_cast<std::uint32_t>(j)};
            const auto [begin, end] = resolvent_idxs_by_hash.equal_range(hash);
            const auto found = std::find_if(begin, end, [&](const auto& entry) {
                return rows_equal(resolution.resolvents[entry.second], by_lead.size(), row_at);
            });
            if (found != end) {
                resolution.parents[found->second] =
                    std::min(resolution.parents[found->second], parents);
                return true;
            }
            rows_added += by_lead.size();
            if (rows_added > rows_removed) {
                return false;
            }
            resolvent_idxs_by_hash.emplace(hash, resolution.resolvents.size());
            Rows& resolvent = resolution.resolvents.emplace_back();
            resolvent.reserve(by_lead.size());
            for (const std::uint32_t row_idx : by_lead) {
                resolvent.push_back(spanning[row_idx]);
            }
            resolution.parents.push_back(parents);
            return true;
        };
        // The pairs least likely to resolve to a tautology first, so that a
        // refusal comes early: a clause outside the gate with each member, then
        // the members with each other; with no gate, clauses far apart in the
        // clause store before near ones.
        if (in_gate.has_value()) {
            std::vector<std::size_t> members;
            for (std::size_t i = 0; i < parts.size(); ++i) {
                if ((*in_gate)[i] != 0) {
                    members.push_back(i);
                }
            }
            for (std::size_t i = 0; i < parts.size(); ++i) {
                if ((*in_gate)[i] != 0) {
                    continue;
                }
                for (const std::size_t member : members) {
                    if (!resolve_pair(std::min(i, member), std::max(i, member))) {
                        return std::nullopt;
                    }
                }
            }
            for (std::size_t a = 0; a < members.size(); ++a) {
                for (std::size_t b = a + 1; b < members.size(); ++b) {
                    if (!resolve_pair(members[a], members[b])) {
                        return std::nullopt;
                    }
                }
            }
        } else {
            const std::size_t num_parts = parts.size();
            const std::size_t middle = num_parts / 2;
            for (std::size_t step = 0; step < 2 * num_parts; ++step) {
                // middle, middle + 1, middle - 1, middle + 2, ...
                const std::size_t offset = (step + 1) / 2;
                if (step % 2 == 0 && offset > middle) {
                    continue;
                }
                const std::size_t distance = step % 2 == 0 ? middle - offset : middle + offset;
                if (distance == 0 || distance >= num_parts) {
                    continue;
                }
                for (std::size_t i = 0; i + distance < num_parts; ++i) {
                    if (!resolve_pair(i, i + distance)) {
                        return std::nullopt;
                    }
                }
            }
        }
        std::vector<std::size_t> by_parents(resolution.resolvents.size());
        std::iota(by_parents.begin(), by_parents.end(), std::size_t{0});
        std::sort(by_parents.begin(), by_parents.end(), [&resolution](std::size_t a, std::size_t b) {
            return resolution.parents[a] < resolution.parents[b];
        });
        Resolution in_order;
        in_order.by_gate = in_gate.has_value();
        for (const std::size_t i : by_parents) {
            in_order.resolvents.push_back(std::move(resolution.resolvents[i]));
            in_order.parents.push_back(
                {clause_idxs[resolution.parents[i][0]], clause_idxs[resolution.parents[i][1]]});
        }
        return in_order;
    }

    /// @brief Add the resolvents, remove @p clause_idxs and record the elimination.
    void replace(const std::vector<std::uint32_t>& clause_idxs, Resolution resolution,
                 std::vector<std::uint32_t> flip_set) {
        Elimination record{.flip_set = std::move(flip_set), .removed_clauses = {}};
        num_eliminated_by_gate_ += resolution.by_gate ? 1U : 0U;
        for (std::size_t i = 0; i < resolution.resolvents.size(); ++i) {
            found_empty_clause_ = found_empty_clause_ || resolution.resolvents[i].empty();
            add(std::move(resolution.resolvents[i]), resolution.parents[i]);
        }
        for (const std::uint32_t clause_idx : clause_idxs) {
            clauses_[clause_idx].elimination_idx = eliminations_.size();
            clauses_[clause_idx].removed_clause_idx = record.removed_clauses.size();
            std::vector<VarListEquation>& removed = record.removed_clauses.emplace_back();
            for (const V& row : remove(clause_idx)) {
                removed.push_back(var_list_of(row));
            }
        }
        eliminations_.push_back(std::move(record));
    }

    /// @brief The members of a gate among @p parts, the clauses @p clause_idxs
    /// split; nothing if their rests are satisfiable together, if the gate solver
    /// reaches its conflict cap, or if they are more than `gate_max_clause_share` of the
    /// present clauses.
    ///
    /// With `config_.root_equations`, a one-sided gate's root equations are added
    /// as unit clauses. `search_gate_hybrid()` reads each rest as its coordinates
    /// over a basis of the rests' directions, with its constant.
    std::optional<std::vector<std::uint8_t>> find_gate(std::span<const SplitClause<V>> parts,
                                                       const std::vector<std::uint32_t>& clause_idxs) {
        if (static_cast<double>(parts.size()) >
            gate_max_clause_share * static_cast<double>(present_clause_idxs_by_hash_.size())) {
            return std::nullopt;
        }
        const auto start = std::chrono::steady_clock::now();
        // The basis in echelon form, each row reduced by those before it.
        std::vector<V> basis;
        std::vector<std::size_t> lead_of_basis;
        // Per rest row, in the order of the parts, the basis rows its direction sums.
        std::vector<std::vector<std::uint32_t>> in_basis;
        for (const SplitClause<V>& part : parts) {
            for (const V& row : part.rests()) {
                direction_.assign_from(row);
                direction_.reset_bit(constant_bit(direction_));
                std::vector<std::uint32_t>& sums = in_basis.emplace_back();
                for (std::uint32_t basis_idx = 0; basis_idx < basis.size(); ++basis_idx) {
                    if (direction_.get_bit(lead_of_basis[basis_idx])) {
                        direction_.plus_assign(basis[basis_idx]);
                        sums.push_back(basis_idx);
                    }
                }
                if (const std::optional<std::size_t> lead = direction_.leading_bit()) {
                    sums.push_back(static_cast<std::uint32_t>(basis.size()));
                    basis.push_back(direction_);
                    lead_of_basis.push_back(*lead);
                }
            }
        }
        const std::size_t rank = basis.size();
        std::vector<std::vector<DenseGF2Vector>> rests(parts.size());
        std::size_t rest_idx = 0;
        for (std::size_t part_idx = 0; part_idx < parts.size(); ++part_idx) {
            for (const V& row : parts[part_idx].rests()) {
                DenseGF2Vector& rest = rests[part_idx].emplace_back(rank + 1);
                for (const std::uint32_t basis_idx : in_basis[rest_idx++]) {
                    rest.set_bit(basis_idx);
                }
                if (constant_of(row)) {
                    rest.set_bit(rank);
                }
            }
        }
        GateSearch search = search_gate_hybrid(rests, rank + 1, gate_max_table_cost,
                                               config_.gate_max_conflicts, gate_table_, rng_);
        gate_time_s_ += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        if (!search.core.has_value()) {
            return std::nullopt;
        }
        const std::vector<std::uint32_t>& core = *search.core;
        if (config_.root_equations) {
            const std::vector<Equation> root_negations = changed_shared_equations_of(parts, core);
            // A table's gate comes without a refutation; the gate solver gives the proof's lines.
            if (with_proof_ && !root_negations.empty() && search.by_table) {
                search.refutation = refute_gate(rests, rank + 1, core, refutation_rng_);
            }
            static const GateRefutation no_refutation;
            const GateRefutation& refutation = with_proof_ ? search.refutation : no_refutation;
            for (const Equation& negation : root_negations) {
                add_root_equation(gf2_vector_of<V>(negation), clause_idxs, basis, refutation);
            }
        }
        std::vector<std::uint8_t> in_gate(parts.size(), 0);
        for (const std::uint32_t part_idx : core) {
            in_gate[part_idx] = 1;
        }
        return in_gate;
    }

    /// @brief A basis of the equations the spans of the linear negations of the
    /// clauses @p core names among @p parts share, each changed by the flip set.
    std::vector<Equation> changed_shared_equations_of(std::span<const SplitClause<V>> parts,
                                                      const std::vector<std::uint32_t>& core) const {
        const auto negation_of = [&](const SplitClause<V>& part, const bool with_changed) {
            Clause clause(V::repr, num_vars_);
            const auto add = [&](const V& row) {
                Equation equation(V::repr, num_vars_);
                gf2_vector_of<V>(equation).assign_from(row);
                clause.add_row(std::move(equation));
            };
            if (with_changed) {
                add(part.changed);
            }
            for (const V& row : part.rests()) {
                add(row);
            }
            return clause;
        };
        std::vector<Clause> negations;
        negations.reserve(core.size());
        for (const std::uint32_t part_idx : core) {
            negations.push_back(negation_of(parts[part_idx], true));
        }
        return changed_shared_equations(negations, negation_of(parts[core[0]], false));
    }

    /// @brief Add the unit clause whose linear negation is @p negation, false at the
    /// root by a one-sided gate among the clauses @p clause_idxs, unless a present
    /// clause has its span; its derivation is @p refutation's lines with @p negation
    /// added to each.
    /// @param basis The directions @p refutation's coordinates stand for.
    void add_root_equation(const V& negation, const std::vector<std::uint32_t>& clause_idxs,
                           const std::vector<V>& basis, const GateRefutation& refutation) {
        std::optional<Rows> unit = canonical_of(Rows{negation});
        assert(unit.has_value() && "a changed equation is 0 = 1");
        if (is_present(*unit)) {
            return;
        }
        const std::size_t rank = basis.size();
        const auto cited = [&](const std::vector<std::uint32_t>& reasons) {
            std::vector<Cited> cited_clauses;
            cited_clauses.reserve(reasons.size());
            for (const std::uint32_t reason : reasons) {
                cited_clauses.push_back(reason < clause_idxs.size()
                                            ? Cited{.is_lemma = false, .idx = clause_idxs[reason]}
                                            : Cited{.is_lemma = true,
                                                    .idx = static_cast<std::uint32_t>(reason - clause_idxs.size())});
            }
            return cited_clauses;
        };
        Derivation derivation;
        for (const GateRefutation::Lemma& lemma : refutation.lemmas) {
            Rows rows{negation};
            for (const DenseGF2Vector& row : lemma.rows) {
                V& lifted = rows.emplace_back(num_vars_ + 1);
                row.for_each_set_bit([&](const std::size_t bit) {
                    if (bit < rank) {
                        lifted.plus_assign(basis[bit]);
                    } else {
                        lifted.set_bit(num_vars_);
                    }
                });
            }
            derivation.lemmas.push_back({.rows = std::move(rows), .reasons = cited(lemma.reasons)});
        }
        derivation.reasons = cited(refutation.reasons);
        const std::uint32_t clause_idx = add(std::move(*unit), {});
        clauses_[clause_idx].derivation_idx = derivations_.size();
        derivations_.push_back(std::move(derivation));
        ++num_root_equations_;
    }

    static constexpr std::uint32_t none = static_cast<std::uint32_t>(-1);

    std::size_t num_vars_;
    BveConfig config_;
    bool with_proof_;
    Rng& rng_;
    Rng refutation_rng_; ///< Seeded from `rng_`, drawn from only for a proof's lines.
    PivotTable leads_; ///< Empty between calls of `canonical()`.
    // Scratch of `resolve()` and `find_gate()`.
    std::vector<SplitClause<V>> parts_;
    Rows spanning_;
    CanonicalWork canonical_work_;
    V direction_; ///< Scratch of `find_gate()`.
    GateTable gate_table_;
    std::size_t num_eliminated_by_gate_ = 0;
    std::size_t num_root_equations_ = 0;
    double gate_time_s_ = 0.0;
    std::vector<Derivation> derivations_;
    std::vector<StoredClause> clauses_; ///< In the order added.
    /// @brief The present clauses' indices, by `hash_of()` their rows.
    std::unordered_multimap<std::size_t, std::uint32_t> present_clause_idxs_by_hash_;
    std::vector<Elimination> eliminations_;
    bool found_empty_clause_ = false;

    std::vector<Direction> directions_;
    std::vector<DirectionId> free_directions_;
    /// @brief The directions in use, by their rows' `hash_except_last_bit()`.
    std::unordered_multimap<std::size_t, DirectionId> direction_ids_by_hash_;

    // Of a pass.
    std::size_t pass_id_ = 0; ///< Passes start from 1.
    bool in_pass_ = false;
    bool has_pass_flip_sets_ = false;
    /// @brief Per direction, the kept rows it sums in the pass `pass_of_` names.
    std::vector<V> pass_sum_;
    std::vector<std::size_t> pass_of_;
    std::vector<std::uint32_t> order_; ///< The present clauses, in the pass's order.
    std::vector<std::uint32_t> adder_of_kept_; ///< Per kept row, the clause that added it.
    /// @brief The kept rows in echelon form, each row with the kept rows it sums.
    PivotTable echelon_leads_;
    std::vector<V> echelon_rows_;
    std::vector<V> pass_sums_;
    std::vector<Var> echelon_lead_of_;
    V reduced_; ///< Scratch of `hold_row()`.
    /// @brief Per kept row, the variables of a flip set that changes it and no
    /// other kept row; made at the pass's first elimination.
    std::vector<V> flip_set_of_kept_;
    /// @brief Per place in `order_`, its clause's first kept row; one more at the end.
    std::vector<std::uint32_t> added_begin_;
    /// @brief Per kept row, the clauses with a row summing it, some no longer present.
    std::vector<std::vector<std::uint32_t>> users_;
    std::vector<std::uint32_t> last_user_; ///< Per kept row, the last clause put on its list.
    std::vector<std::uint32_t> group_of_kept_; ///< Per kept row, `none` in the core subspace.
    std::vector<std::uint32_t> group_of_clause_; ///< Per clause, the group it is a leftover of.
    std::vector<std::uint32_t> group_parent_; ///< Union-find over groups.
    std::vector<std::uint32_t> group_size_; ///< Of a root, its group rows.
    std::vector<std::size_t> group_mark_;
    /// @brief The rows of the clauses held in the core subspace, as vectors of kept
    /// rows in reduced form, each led by its highest kept row.
    std::vector<V> held_;
    std::vector<std::uint32_t> held_pivot_; ///< Per held vector, the kept row leading it.
    IndexSet held_pivots_;
    std::vector<std::uint32_t> held_of_pivot_; ///< Per kept row, the held vector it leads.
    /// @brief Per kept row, held vectors that have it; some no longer do.
    std::vector<std::vector<std::uint32_t>> held_with_;
    V beyond_held_; ///< Scratch of `beyond_held_in_place()`.
    std::vector<std::uint8_t> group_row_parity_; ///< Scratch of the walk.
    /// @brief Per kept row, its place among the rows of the group in hand; `none` if not one.
    std::vector<std::uint32_t> unknown_idx_of_kept_;
    std::vector<std::size_t> clause_mark_;
    std::size_t mark_stamp_ = 0;
    std::vector<std::uint32_t> label_; ///< Per direction, see `label_of()`.
    std::vector<std::size_t> label_at_;
    std::size_t label_stamp_ = 0;
};

template <GF2Vector V>
Clause clause_of(const std::vector<V>& rows, std::size_t num_vars, EquationRepr repr) {
    Clause clause(repr, num_vars);
    for (const V& row : rows) {
        Equation equation(repr, num_vars);
        gf2_vector_of<V>(equation).assign_from(row);
        clause.add_row(std::move(equation));
    }
    assert(clause.rows() == rows.size() && "a canonical clause lost a row");
    return clause;
}

template <GF2Vector V>
BvePlan build_bve_plan_over(const std::vector<Clause>& clauses, const ActiveClauseIds active_clause_ids,
                            const std::size_t num_vars, const EquationRepr repr, const BveConfig& config,
                            const bool with_proof, Rng& rng) {
    using Rows = std::vector<V>;
    BvePlan plan;
    Formula<V> formula(num_vars, config, with_proof, rng);
    BveCounters& counters = plan.counters;
    for (const ClauseId id : active_clause_ids) {
        assert(clauses[id].num_vars() == num_vars && "an active clause is over another numbering");
        Rows spanning;
        spanning.reserve(clauses[id].rows());
        for (std::size_t row_idx = 0; row_idx < clauses[id].rows(); ++row_idx) {
            spanning.push_back(gf2_vector_of<V>(clauses[id].row(row_idx)));
        }
        std::optional<Rows> rows = formula.canonical_of(std::move(spanning));
        assert(rows.has_value() && "an active clause is a tautology");
        ++counters.clauses_before;
        counters.width_before += rows->size();
        formula.add_input_clause(std::move(*rows), id);
    }
    const std::size_t num_input_clauses = formula.clauses().size();
    const IndexSet mentioned_before = formula.mentioned_vars();
    formula.run_pass();

    plan.found_empty_clause = formula.found_empty_clause();
    counters.num_eliminated_by_gate = formula.num_eliminated_by_gate();
    counters.num_root_equations = formula.num_root_equations();
    counters.gate_time_s = formula.gate_time_s();
    plan.eliminations = formula.eliminations();
    counters.num_eliminated = plan.eliminations.size();
    plan.dropped_col_idxs = IndexSet(num_vars + 1);
    if (!plan.eliminations.empty()) {
        const IndexSet mentioned_after = formula.mentioned_vars();
        mentioned_before.for_each_set([&](const std::size_t var) {
            if (!mentioned_after.test(var)) {
                plan.dropped_col_idxs.set(var);
            }
        });
    }

    for (std::size_t clause_idx = 0; clause_idx < formula.clauses().size(); ++clause_idx) {
        const typename Formula<V>::StoredClause& clause = formula.clauses()[clause_idx];
        if (clause.present) {
            ++counters.clauses_after;
            counters.width_after += clause.rows.size();
        }
        if (clause_idx < num_input_clauses) {
            if (!clause.present) {
                plan.removed_ids.push_back(clause.input_id);
            }
            continue;
        }
        const auto parent = [&](const std::uint32_t parent_clause_idx) {
            return parent_clause_idx < num_input_clauses
                       ? BvePlan::Parent{.kind = BvePlan::Parent::Kind::clause,
                                         .idx = formula.clauses()[parent_clause_idx].input_id}
                       : BvePlan::Parent{.kind = BvePlan::Parent::Kind::resolvent,
                                         .idx = parent_clause_idx - num_input_clauses};
        };
        BvePlan::Resolvent& resolvent = plan.resolvents.emplace_back(BvePlan::Resolvent{
            .parents = {parent(clause.parent_clause_idxs[0]), parent(clause.parent_clause_idxs[1])},
            .lemmas = {},
            .reasons = {},
            .clause = std::nullopt,
            .elimination_idx = clause.elimination_idx,
            .removed_clause_idx = clause.removed_clause_idx});
        if (clause.derivation_idx.has_value()) {
            const auto cited = [&](const std::vector<typename Formula<V>::Cited>& reasons) {
                std::vector<BvePlan::Parent> parents;
                parents.reserve(reasons.size());
                for (const typename Formula<V>::Cited& reason : reasons) {
                    parents.push_back(reason.is_lemma
                                          ? BvePlan::Parent{.kind = BvePlan::Parent::Kind::lemma, .idx = reason.idx}
                                          : parent(reason.idx));
                }
                return parents;
            };
            const typename Formula<V>::Derivation& derivation = formula.derivations()[*clause.derivation_idx];
            for (const typename Formula<V>::Derivation::Lemma& lemma : derivation.lemmas) {
                resolvent.lemmas.push_back({.clause = clause_of<V>(lemma.rows, num_vars, repr),
                                            .reasons = cited(lemma.reasons)});
            }
            resolvent.reasons = cited(derivation.reasons);
        }
        if (clause.present) {
            counters.num_unit_resolvents += clause.rows.size() == 1 ? 1U : 0U;
            counters.num_resolvents_of_width_at_most_2 += clause.rows.size() <= 2 ? 1U : 0U;
            resolvent.clause = clause_of<V>(clause.rows, num_vars, repr);
        }
    }
    std::ranges::sort(plan.removed_ids);
    return plan;
}

} // namespace

std::vector<Equation> changed_shared_equations(const std::vector<Clause>& negations,
                                               const Clause& unchanged) {
    assert(!negations.empty() && "no clauses to share an equation");
    Clause shared = negations[0];
    for (std::size_t i = 1; i < negations.size() && shared.rows() > 0; ++i) {
        std::vector<Equation> rows = span_intersection(shared, negations[i]);
        shared = Clause(shared.repr(), shared.num_vars());
        for (Equation& row : rows) {
            shared.add_row(std::move(row));
        }
    }
    // Each row the flip set does not change is replaced by its sum with one it does.
    std::vector<Equation> changed;
    std::optional<Equation> first_changed;
    std::vector<Equation> unchanged_rows;
    for (const Equation& row : shared) {
        Equation reduced = row;
        if (unchanged.partial_reduce(reduced).kind == ReduceResult::Kind::eq_true) {
            unchanged_rows.push_back(row);
            continue;
        }
        if (!first_changed.has_value()) {
            first_changed = row;
        }
        changed.push_back(row);
    }
    if (!first_changed.has_value()) {
        return {};
    }
    for (Equation& row : unchanged_rows) {
        row.plus_assign(*first_changed);
        changed.push_back(std::move(row));
    }
    return changed;
}

BvePlan build_bve_plan(const std::vector<Clause>& clauses,
                       const ActiveClauseIds active_clause_ids, const std::size_t num_vars,
                       const EquationRepr repr, const BveConfig& config, const bool with_proof, Rng& rng) {
    assert(config.group_cap >= 1 && config.group_cap <= 16 && config.max_refusals_in_a_row >= 1 &&
           config.success_rate >= 0.0 && config.success_rate <= 1.0 && config.gate_max_conflicts >= 1 &&
           "the group cap, the refusal limit, the success rate or the gate conflict cap is out of range");
    return with_gf2_vector_of(repr, [&]<GF2Vector V>(std::type_identity<V>) {
        return build_bve_plan_over<V>(clauses, active_clause_ids, num_vars, repr, config, with_proof, rng);
    });
}

} // namespace xorcle
