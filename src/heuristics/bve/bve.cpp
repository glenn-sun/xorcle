#include "heuristics/bve/bve.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <numeric>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "algebra/equation.h"
#include "algebra/equation_template.h"
#include "algebra/gf2_vector.h"
#include "algebra/gf2_vector_of_repr.h"
#include "algebra/reduce_result.h"
#include "util/index_set.h"
#include "util/random.h"

namespace xorcle {

namespace {

using Var = std::uint32_t;
constexpr Var no_var = static_cast<Var>(-1);
constexpr std::uint32_t no_basis_idx = static_cast<std::uint32_t>(-1);

/// @brief Index of a direction: an equation without its constant.
using DirectionId = std::uint32_t;

/// @brief Independent equations a gate's truth table may range over.
constexpr std::size_t max_gate_rank = 15;
/// @brief Steps reading or writing an equation costs before its length is
/// counted: an equation of no length takes as long as 16 units of a long one.
constexpr std::size_t steps_per_equation = 16;
/// @brief Truth-table words read or written in one step.
constexpr std::size_t table_words_per_step = 4;

/// @brief What reading or writing a vector once costs, in steps: the words its bits take.
template <GF2Vector V>
std::size_t length(const V& vector) {
    return (vector.num_bits() + 63) / 64;
}

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
    const std::size_t bit = constant_bit(row);
    if (row.get_bit(bit)) {
        row.reset_bit(bit);
    } else {
        row.set_bit(bit);
    }
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

template <GF2Vector V>
void plus_assign(V& row, const V& other, std::size_t& steps) {
    steps += length(row) + steps_per_equation;
    row.plus_assign(other);
}

template <GF2Vector V>
void append_key(std::string& key, const V& row) {
    key.push_back(constant_of(row) ? '\1' : '\0');
    row.append_key_except_last_bit(key);
    // Ends the row, so that keys of several rows split one way only.
    key.append(sizeof(std::uint32_t), '\xff');
}

/// @brief A key of the equation's direction: of its variables, without its constant.
template <GF2Vector V>
void append_direction_key(std::string& key, const V& row) {
    row.append_key_except_last_bit(key);
}

template <GF2Vector V>
VarListEquation var_list_of(const V& row) {
    VarListEquation out;
    out.constant = constant_of(row);
    for_each_var(row, [&out](const Var var) { out.vars.push_back(var); });
    return out;
}

/// @brief The steps of reading or writing @p rows once.
template <GF2Vector V>
std::size_t steps_of(const std::vector<V>& rows) {
    std::size_t steps = rows.size() * steps_per_equation;
    for (const V& row : rows) {
        steps += length(row);
    }
    return steps;
}

template <GF2Vector V>
std::string key_of(const std::vector<V>& rows, std::size_t& steps) {
    steps += steps_of(rows);
    std::string key;
    for (const V& row : rows) {
        append_key(key, row);
    }
    return key;
}

template <GF2Vector V>
std::size_t equation_bits(const std::vector<V>& rows) {
    std::size_t bits = 0;
    for (const V& row : rows) {
        bits += num_vars_of(row);
    }
    return bits;
}

/// @brief The leading variables of a basis, each with the index of its row.
struct Leads {
    explicit Leads(std::size_t num_vars) : basis_idx_of(num_vars, no_basis_idx), is_lead(num_vars + 1) {}

    void set(Var var, std::size_t basis_idx) {
        basis_idx_of[var] = static_cast<std::uint32_t>(basis_idx);
        is_lead.set(var);
    }

    void reset(Var var) {
        basis_idx_of[var] = no_basis_idx;
        is_lead.reset(var);
    }

    /// @brief The lowest leading variable @p row mentions that is at least
    /// @p from; `no_var` if none.
    template <GF2Vector V>
    Var lowest_in(const V& row, Var from) const {
        const std::optional<std::size_t> bit = row.first_bit_in(is_lead, from);
        return bit.has_value() ? static_cast<Var>(*bit) : no_var;
    }

    std::vector<std::uint32_t> basis_idx_of; ///< Per variable; `no_basis_idx` if it leads no row.
    IndexSet is_lead; ///< Over the bits of an equation.
};

/// @brief A uniform draw from `[0, bound)`, by rejection, so that the standard
/// library does not decide it; nothing is drawn when `bound` is 1.
/// @pre `bound > 0`.
std::size_t uniform_below(Rng& rng, std::size_t bound) {
    assert(bound > 0 && "a draw needs something to draw from");
    if (bound == 1) {
        return 0;
    }
    const std::uint64_t mask = std::bit_ceil(static_cast<std::uint64_t>(bound)) - 1;
    while (true) {
        const std::uint64_t draw = rng() & mask;
        if (draw < bound) {
            return static_cast<std::size_t>(draw);
        }
    }
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
bool canonical_in_place(std::span<V> rows, Leads& leads, CanonicalWork& work, std::size_t& steps) {
    work.basis.clear();
    work.lead_of.clear();
    bool holds_eq_false = false;
    // A row of the basis mentions no variable leading an earlier row.
    for (std::size_t i = 0; i < rows.size(); ++i) {
        V& row = rows[i];
        steps += length(row) + steps_per_equation;
        for (Var lead = leads.lowest_in(row, 0); lead != no_var;
             lead = leads.lowest_in(row, lead + 1)) {
            const std::uint32_t row_idx = work.basis[leads.basis_idx_of[lead]];
            plus_assign(row, rows[row_idx], steps);
        }
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
            steps += length(row) + steps_per_equation;
            for (Var lead = leads.lowest_in(row, work.lead_of[*i] + 1); lead != no_var;
                 lead = leads.lowest_in(row, lead + 1)) {
                const std::uint32_t row_idx = work.basis[leads.basis_idx_of[lead]];
                plus_assign(row, rows[row_idx], steps);
            }
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
std::optional<std::vector<V>> canonical(std::vector<V> spanning, Leads& leads, std::size_t& steps) {
    CanonicalWork work;
    if (!canonical_in_place(std::span<V>(spanning), leads, work, steps)) {
        return std::nullopt;
    }
    std::vector<V> in_order;
    in_order.reserve(work.by_lead.size());
    for (const std::uint32_t row_idx : work.by_lead) {
        in_order.push_back(std::move(spanning[row_idx]));
    }
    return in_order;
}

/// @brief The truth table, in coordinates of a basis of their equations, of
/// the points some clause added so far forbids.
template <GF2Vector V>
class GateTable {
public:
    enum class Outcome { over_rank, open, covered };

    /// @param num_bits Of the equations added.
    explicit GateTable(std::size_t num_bits) : rest_(num_bits) {}

    /// @brief Start again from no clause added, keeping the buffers.
    void clear() {
        rank_ = 0;
        forbidden_.assign(1, 0);
        num_added_ = 0;
    }

    /// @brief Add the clause with linear negation @p negation, unless its
    /// equations take the basis past `max_gate_rank`; `covered` once the
    /// clauses added are jointly unsatisfiable.
    Outcome add(std::span<const V> negation, std::size_t& steps) {
        const std::size_t rank_before = rank_;
        if (coordinates_of_added_.size() == num_added_) {
            coordinates_of_added_.emplace_back();
        }
        Coordinates& coordinates = coordinates_of_added_[num_added_];
        coordinates.clear();
        for (const V& row : negation) {
            rest_.assign_from(row);
            rest_.reset_bit(constant_bit(rest_));
            steps += length(rest_) + steps_per_equation;
            std::uint32_t in_basis = 0;
            for (Var leading_var = lowest_var_from(rest_, 0); leading_var != no_var;
                 leading_var = lowest_var_from(rest_, leading_var + 1)) {
                std::size_t basis_idx = 0;
                while (basis_idx < rank_ && lead_of_basis_[basis_idx] != leading_var) {
                    ++basis_idx;
                }
                if (basis_idx == rank_) {
                    if (rank_ == max_gate_rank) {
                        rank_ = rank_before;
                        return Outcome::over_rank;
                    }
                    lead_of_basis_[rank_] = leading_var;
                    if (basis_.size() == rank_) {
                        basis_.emplace_back(rest_.num_bits());
                    }
                    basis_[rank_].assign_from(rest_);
                    ++rank_;
                    steps += length(rest_) + steps_per_equation;
                }
                plus_assign(rest_, basis_[basis_idx], steps);
                in_basis ^= std::uint32_t{1} << basis_idx;
            }
            coordinates.emplace_back(in_basis, constant_of(row));
        }

        // One word holds six coordinates, and a table over fewer repeats.
        const std::size_t num_words =
            std::size_t{1} << (std::max<std::size_t>(rank_, 6) - 6);
        while (forbidden_.size() < num_words) {
            forbidden_.insert(forbidden_.end(), forbidden_.begin(), forbidden_.end());
        }
        steps += num_words * (coordinates.size() + 1) / table_words_per_step + 1;
        split_of(coordinates, split_);
        bool covered = true;
        for (std::size_t word_idx = 0; word_idx < num_words; ++word_idx) {
            forbidden_[word_idx] |= forbidden_word(split_, word_idx);
            covered = covered && forbidden_[word_idx] == ~std::uint64_t{0};
        }
        ++num_added_;
        return covered ? Outcome::covered : Outcome::open;
    }

    /// @brief The members left once each one the others cover without is taken
    /// out, tried in the order added.
    /// @pre The clauses added are jointly unsatisfiable.
    std::vector<std::uint8_t> minimal_cover(std::size_t& steps) {
        const std::size_t num_added = num_added_;
        const std::size_t num_words = forbidden_.size();
        std::vector<std::uint64_t> forbidden_by(num_added * num_words);
        for (std::size_t added = 0; added < num_added; ++added) {
            split_of(coordinates_of_added_[added], split_);
            for (std::size_t word_idx = 0; word_idx < num_words; ++word_idx) {
                forbidden_by[added * num_words + word_idx] = forbidden_word(split_, word_idx);
            }
            steps += num_words * (coordinates_of_added_[added].size() + 1) / table_words_per_step + 1;
        }
        std::vector<std::uint8_t> is_member(num_added, 1);
        std::size_t num_members = num_added;
        for (std::size_t added = 0; added < num_added && num_members > 1; ++added) {
            bool others_cover = true;
            for (std::size_t word_idx = 0; word_idx < num_words && others_cover; ++word_idx) {
                std::uint64_t forbidden = 0;
                for (std::size_t other = 0; other < num_added; ++other) {
                    if (other != added && is_member[other] != 0) {
                        forbidden |= forbidden_by[other * num_words + word_idx];
                    }
                }
                others_cover = forbidden == ~std::uint64_t{0};
            }
            steps += num_words * num_added / table_words_per_step + 1;
            if (others_cover) {
                is_member[added] = 0;
                --num_members;
            }
        }
        return is_member;
    }

private:
    using Coordinates = std::vector<std::pair<std::uint32_t, bool>>;

    /// @brief An equation's coordinates as the part within one word, with the
    /// constant folded in, and the coordinates that pick the word.
    struct Split {
        std::uint64_t in_word;
        std::uint32_t of_word;
    };

    static void split_of(const Coordinates& coordinates, std::vector<Split>& split) {
        split.clear();
        for (const auto& [in_basis, constant] : coordinates) {
            std::uint64_t sum = 0;
            for (std::uint32_t rest = in_basis & 63U; rest != 0; rest &= rest - 1) {
                sum ^= coordinate_word(static_cast<std::size_t>(std::countr_zero(rest)), 0);
            }
            split.push_back({.in_word = constant ? sum : ~sum, .of_word = in_basis >> 6});
        }
    }

    /// @brief Word @p word_idx of the points at which every equation of a
    /// linear negation with these coordinates holds.
    static std::uint64_t forbidden_word(const std::vector<Split>& split, std::size_t word_idx) {
        std::uint64_t forbidden = ~std::uint64_t{0};
        for (const Split& equation : split) {
            const bool flips = (std::popcount(equation.of_word & static_cast<std::uint32_t>(word_idx)) & 1) != 0;
            forbidden &= flips ? ~equation.in_word : equation.in_word;
        }
        return forbidden;
    }

    /// @brief Word @p word_idx of the truth table of coordinate @p coordinate.
    static std::uint64_t coordinate_word(std::size_t coordinate, std::size_t word_idx) {
        constexpr std::array<std::uint64_t, 6> in_word{
            0xaaaaaaaaaaaaaaaaULL, 0xccccccccccccccccULL, 0xf0f0f0f0f0f0f0f0ULL,
            0xff00ff00ff00ff00ULL, 0xffff0000ffff0000ULL, 0xffffffff00000000ULL};
        if (coordinate < 6) {
            return in_word[coordinate];
        }
        return ((word_idx >> (coordinate - 6)) & 1U) != 0 ? ~std::uint64_t{0} : 0;
    }

    std::vector<V> basis_; ///< Without constants; its first `rank_` rows.
    std::size_t rank_ = 0;
    std::array<Var, max_gate_rank> lead_of_basis_{};
    std::vector<std::uint64_t> forbidden_{0};
    std::vector<Coordinates> coordinates_of_added_; ///< Its first `num_added_`.
    std::size_t num_added_ = 0;
    V rest_;
    std::vector<Split> split_;
};

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

/// @brief Bits over the unknowns of one flip-set search.
using UnknownBits = std::vector<std::uint64_t>;

bool any(const UnknownBits& bits) {
    return std::ranges::any_of(bits, [](std::uint64_t word) { return word != 0; });
}

bool test(const UnknownBits& bits, std::size_t idx) {
    return ((bits[idx >> 6] >> (idx & 63)) & 1U) != 0;
}

void flip(UnknownBits& bits, std::size_t idx) { bits[idx >> 6] ^= std::uint64_t{1} << (idx & 63); }

/// @pre `any(bits)`.
std::size_t lowest(const UnknownBits& bits) {
    for (std::size_t word_idx = 0; word_idx < bits.size(); ++word_idx) {
        if (bits[word_idx] != 0) {
            return (word_idx << 6) + static_cast<std::size_t>(std::countr_zero(bits[word_idx]));
        }
    }
    assert(false && "no bit is set");
    return 0;
}

void plus_assign(UnknownBits& bits, const UnknownBits& other) {
    for (std::size_t word_idx = 0; word_idx < bits.size(); ++word_idx) {
        bits[word_idx] ^= other[word_idx];
    }
}

/// @brief Whether @p a and @p b share an odd number of bits.
bool odd_overlap(const UnknownBits& a, const UnknownBits& b) {
    std::uint64_t parity = 0;
    for (std::size_t word_idx = 0; word_idx < a.size(); ++word_idx) {
        parity ^= a[word_idx] & b[word_idx];
    }
    return (std::popcount(parity) & 1) != 0;
}

bool odd(std::uint32_t mask) { return (std::popcount(mask) & 1) != 0; }

void plus_or(std::vector<std::uint64_t>& bits, const std::vector<std::uint64_t>& other) {
    for (std::size_t word_idx = 0; word_idx < bits.size(); ++word_idx) {
        bits[word_idx] |= other[word_idx];
    }
}

/// @brief The clauses of a formula in canonical form, and of a reading its kept rows.
///
/// A sum of a reading's kept rows is a vector over the kept rows, of `num_vars` bits
/// as the kept rows are independent.
template <GF2Vector V>
class Formula {
public:
    using Rows = std::vector<V>;

    struct StoredClause {
        Rows rows; ///< In rising order of their lowest variables.
        std::vector<DirectionId> row_directions;
        ClauseId input_id = 0;
        std::vector<ClauseId> duplicate_ids; ///< Input clauses equal to this one.
        std::array<std::uint32_t, 2> parent_clause_idxs{}; ///< Of a resolvent.
        /// @brief Once removed, the elimination that removed it and its place
        /// among that elimination's removed clauses.
        std::size_t elimination_idx = 0;
        std::size_t removed_clause_idx = 0;
        bool present = true;
    };

    Formula(std::size_t num_vars, const BveConfig& config, Rng& rng)
        : num_vars_(num_vars),
          config_(config),
          rng_(rng),
          leads_(num_vars),
          gate_table_(num_vars + 1),
          echelon_leads_(num_vars),
          reduced_(num_vars + 1),
          held_pivots_(num_vars),
          beyond_held_(num_vars) {}

    const std::vector<StoredClause>& clauses() const noexcept { return clauses_; }
    const std::vector<Elimination>& eliminations() const noexcept { return eliminations_; }
    bool found_empty_clause() const noexcept { return found_empty_clause_; }
    std::size_t steps() const noexcept { return steps_; }
    BveCounters& counters() noexcept { return counters_; }

    /// @brief The canonical form of the clause with linear negation spanned by
    /// @p spanning; nothing if the span holds `0 = 1`.
    std::optional<Rows> canonical_of(Rows spanning) {
        return canonical(std::move(spanning), leads_, steps_);
    }

    /// @brief Add an input clause, or record @p id as the duplicate of an equal one already there.
    /// @pre `rows` is canonical.
    void add_input_clause(Rows rows, ClauseId id) {
        const auto found = seen_.find(key_of(rows, steps_));
        if (found != seen_.end()) {
            clauses_[found->second].duplicate_ids.push_back(id);
            return;
        }
        const std::uint32_t clause_idx = add(std::move(rows), {});
        clauses_[clause_idx].input_id = id;
    }

    /// @brief Read the clauses in uniformly random orders, eliminating in each
    /// reading's tail groups and then each clause some flip set touches alone,
    /// until a reading removes fewer than `success_rate` of the clauses present
    /// at its start, or none.
    void run_projection() {
        std::size_t num_present = num_present_clauses();
        while (!found_empty_clause_) {
            if (read_and_eliminate()) {
                ++counters_.num_readings_eliminating;
            }
            const std::size_t num_present_before = num_present;
            num_present = num_present_clauses();
            if (num_present >= num_present_before ||
                static_cast<double>(num_present_before - num_present) <
                    config_.success_rate * static_cast<double>(num_present_before)) {
                break;
            }
        }
    }

    std::size_t num_present_clauses() const {
        return static_cast<std::size_t>(
            std::ranges::count_if(clauses_, [](const StoredClause& clause) { return clause.present; }));
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
        std::uint32_t num_rows = 0; ///< The clause rows with this direction.
    };

    // --- The clauses -----------------------------------------------------------

    /// @return The clause's index.
    std::uint32_t add(Rows rows, std::array<std::uint32_t, 2> parent_clause_idxs) {
        const std::uint32_t clause_idx = static_cast<std::uint32_t>(clauses_.size());
        seen_.emplace(key_of(rows, steps_), clause_idx);
        StoredClause clause;
        clause.rows = std::move(rows);
        clause.parent_clause_idxs = parent_clause_idxs;
        const std::size_t width = clause.rows.size();
        clause.row_directions.reserve(width);
        for (std::size_t row_idx = 0; row_idx < width; ++row_idx) {
            const DirectionId direction = intern(clause.rows[row_idx]);
            clause.row_directions.push_back(direction);
            hold_row(direction);
        }
        if (clause.rows.size() == 1) {
            unit_clause_idxs_.push_back(clause_idx);
        }
        clauses_.push_back(std::move(clause));
        if (is_reading_) {
            register_users(clause_idx);
        }
        return clause_idx;
    }

    Rows remove(std::uint32_t clause_idx) {
        StoredClause& clause = clauses_[clause_idx];
        assert(clause.present && "the clause was removed already");
        clause.present = false;
        seen_.erase(key_of(clause.rows, steps_));
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

    // --- The directions ------------------------------------------------------------

    /// @brief The id of the direction of @p row, new if no clause has it yet.
    DirectionId intern(const V& row) {
        steps_ += length(row) + steps_per_equation;
        direction_key_.clear();
        append_direction_key(direction_key_, row);
        const auto found = direction_of_key_.find(direction_key_);
        if (found != direction_of_key_.end()) {
            return found->second;
        }
        DirectionId direction;
        if (!free_directions_.empty()) {
            direction = free_directions_.back();
            free_directions_.pop_back();
        } else {
            direction = static_cast<DirectionId>(directions_.size());
            directions_.emplace_back();
            reading_sum_.emplace_back(num_vars_);
            reading_of_.push_back(0);
            label_.push_back(0);
            label_at_.push_back(0);
        }
        V& vars = directions_[direction].vars.emplace(row);
        vars.reset_bit(constant_bit(vars));
        direction_of_key_.emplace(direction_key_, direction);
        return direction;
    }

    /// @brief Count a clause row with this direction; in a reading, a row new
    /// to it is a sum of rows read, whose kept rows are found by reduction.
    void hold_row(DirectionId direction) {
        Direction& entry = directions_[direction];
        if (entry.num_rows++ == 0 && is_reading_ && reading_of_[direction] != reading_id_) {
            reduced_.assign_from(*entry.vars);
            reduce_in_reading(reduced_, reading_sum_[direction]);
            assert(lowest_var_from(reduced_, 0) == no_var && "a new row is not in the span read");
            reading_of_[direction] = reading_id_;
        }
    }

    /// @brief Let go of a direction no clause row has.
    void forget_if_unused(DirectionId direction) {
        Direction& entry = directions_[direction];
        if (entry.num_rows != 0 || !entry.vars.has_value()) {
            return;
        }
        direction_key_.clear();
        append_direction_key(direction_key_, *entry.vars);
        direction_of_key_.erase(direction_key_);
        entry.vars.reset();
        reading_of_[direction] = 0;
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
            for (Var lead = echelon_leads_.lowest_in(echelon_rows_[i], echelon_lead_of_[i] + 1);
                 lead != no_var; lead = echelon_leads_.lowest_in(echelon_rows_[i], lead + 1)) {
                const std::uint32_t echelon_idx = echelon_leads_.basis_idx_of[lead];
                plus_assign(echelon_rows_[i], echelon_rows_[echelon_idx], steps_);
                plus_assign(reading_sums_[i], reading_sums_[echelon_idx], steps_);
            }
        }
        std::vector<std::vector<std::size_t>> bits_of(num_kept);
        // In rising order of the leading variables, so that each list is ascending.
        for (auto i = by_lead.rbegin(); i != by_lead.rend(); ++i) {
            reading_sums_[*i].for_each_set_bit(
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
        reading_sums_ = {};
        echelon_lead_of_ = {};
    }

    // --- A reading -------------------------------------------------------------------

    /// @brief One reading: span the clauses in a uniformly random order, split
    /// the span by the walk back from the order's end, eliminate in each tail
    /// group, then remove each clause some flip set touches alone.
    /// @return Whether it eliminated anything.
    bool read_and_eliminate() {
        ++reading_id_;
        ++counters_.num_readings;
        order_.clear();
        for (std::uint32_t clause_idx = 0; clause_idx < clauses_.size(); ++clause_idx) {
            if (clauses_[clause_idx].present) {
                order_.push_back(clause_idx);
            }
        }
        if (order_.empty()) {
            return false;
        }
        for (std::size_t i = order_.size(); i > 1; --i) {
            std::swap(order_[i - 1], order_[uniform_below(rng_, i)]);
        }
        const std::size_t steps_before = steps_;
        span_in_order();
        counters_.steps_to_keep_rows += steps_ - steps_before;
        steps_ = steps_before;
        is_reading_ = true;
        has_reading_flip_sets_ = false;

        bool eliminated = false;
        substitute_units();
        if (!found_empty_clause_) {
            walk_back();
            eliminated = eliminate_in_tail_groups();
        }
        if (!found_empty_clause_) {
            eliminated = eliminate_clauses_touched_alone() || eliminated;
        }

        is_reading_ = false;
        clear_echelon();
        return eliminated;
    }

    /// @brief Reduce each clause's rows, in `order_`, against the span of the
    /// rows kept so far, keeping each that is not in it, and record for each row
    /// the kept rows it sums and for each kept row the clauses using it.
    void span_in_order() {
        adder_of_kept_.clear();
        added_begin_.clear();
        for (const std::uint32_t clause_idx : order_) {
            added_begin_.push_back(static_cast<std::uint32_t>(adder_of_kept_.size()));
            for (const DirectionId direction : clauses_[clause_idx].row_directions) {
                if (reading_of_[direction] == reading_id_) {
                    continue;
                }
                reading_of_[direction] = reading_id_;
                V& sum_of_row = reading_sum_[direction];
                V rest = *directions_[direction].vars;
                reduce_in_reading(rest, sum_of_row);
                const Var lead = lowest_var_from(rest, 0);
                if (lead == no_var) {
                    continue;
                }
                const auto kept_idx = static_cast<std::uint32_t>(adder_of_kept_.size());
                adder_of_kept_.push_back(clause_idx);
                // The row is the part reduced away plus itself.
                steps_ += length(sum_of_row) + steps_per_equation;
                sum_of_row.set_bit(kept_idx);
                reading_sums_.push_back(sum_of_row);
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
        kept_mark_.assign(num_kept, 0);
        for (const std::uint32_t clause_idx : order_) {
            register_users(clause_idx);
        }
    }

    /// @brief Reduce @p rest against the reading's echelon, setting @p sum_of_row
    /// to the kept rows of the part reduced away.
    void reduce_in_reading(V& rest, V& sum_of_row) {
        steps_ += length(rest) + steps_per_equation;
        sum_of_row.set_zero();
        for (Var lead = echelon_leads_.lowest_in(rest, 0); lead != no_var;
             lead = echelon_leads_.lowest_in(rest, lead + 1)) {
            const std::uint32_t echelon_idx = echelon_leads_.basis_idx_of[lead];
            plus_assign(rest, echelon_rows_[echelon_idx], steps_);
            plus_assign(sum_of_row, reading_sums_[echelon_idx], steps_);
        }
    }

    /// @brief Put clause @p clause_idx on the list of each kept row its rows sum.
    void register_users(std::uint32_t clause_idx) {
        for (const DirectionId direction : clauses_[clause_idx].row_directions) {
            steps_ += length(reading_sum_[direction]);
            reading_sum_[direction].for_each_set_bit([&](const std::size_t kept_idx) {
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
    /// core span, in a kept buffer valid until the next call: @p sum plus the held
    /// vector of each held pivot it has, which leaves no held pivot.
    const V& beyond_held_in_place(const V& sum) {
        V& rest = beyond_held_;
        rest.assign_from(sum);
        steps_ += length(sum);
        for (std::uint32_t kept_idx = held_pivot_from(sum, 0); kept_idx != none;
             kept_idx = held_pivot_from(sum, kept_idx + 1)) {
            plus_assign(rest, held_[held_of_pivot_[kept_idx]], steps_);
        }
        return rest;
    }

    /// @brief Add the row that sums @p sum to the span of the clauses held in
    /// the core span, kept in reduced form: each held vector led by its highest
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
                plus_assign(other, vector, steps_);
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
        steps_ += users_[kept_idx].size() + held_with_[kept_idx].size();
        for (const std::uint32_t clause_idx : users_[kept_idx]) {
            f(clause_idx);
        }
        for (const std::uint32_t held_idx : held_with_[kept_idx]) {
            if (held_[held_idx].get_bit(kept_idx)) {
                const std::uint32_t pivot = held_pivot_[held_idx];
                steps_ += users_[pivot].size();
                for (const std::uint32_t clause_idx : users_[pivot]) {
                    f(clause_idx);
                }
            }
        }
    }

    /// @brief Split the span into the core span and tail groups by moving a cut
    /// back from the end of `order_`.
    ///
    /// The rows a clause past the cut kept, less the span of the clauses held in
    /// the core span, become tail rows, joined with the tail groups of the
    /// clauses whose rows sum them less that span; unless the tail group would
    /// hold more than the cap, when the clause is held in the core span instead.
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
        std::vector<std::uint32_t> tail;
        std::vector<std::uint32_t> affected;
        std::size_t first_pos = order_.size();
        for (std::size_t pos = order_.size(); pos-- > 0;) {
            const std::uint32_t begin = added_begin_[pos];
            const std::uint32_t end = added_begin_[pos + 1];
            tail.clear();
            for (std::uint32_t kept_idx = begin; kept_idx < end; ++kept_idx) {
                if (held_of_pivot_[kept_idx] == none) {
                    tail.push_back(kept_idx);
                }
            }
            if (tail.empty()) {
                continue;
            }
            const auto is_tail = [&](std::uint32_t kept_idx) {
                return kept_idx >= begin && kept_idx < end && held_of_pivot_[kept_idx] == none;
            };
            // Whether a row's kept rows less the held span have a tail row: the
            // parity of each tail row over the row's kept rows and the held
            // vectors of its held pivots, read only within the clause's kept rows.
            // Calls f with each kept row of `vector` among the clause's own, [begin, end).
            const auto for_each_in_clause = [&](const V& vector, auto&& f) {
                for (Var kept_idx = lowest_bit_from(vector, begin); kept_idx != no_var && kept_idx < end;
                     kept_idx = lowest_bit_from(vector, kept_idx + 1)) {
                    f(kept_idx);
                }
            };
            const auto sums_tail = [&](std::uint32_t clause_idx) {
                return std::ranges::any_of(clauses_[clause_idx].row_directions, [&](DirectionId direction) {
                    const V& sum = reading_sum_[direction];
                    steps_ += length(sum);
                    if (!has_held_pivot(sum)) {
                        bool found = false;
                        for_each_in_clause(sum, [&](const Var kept_idx) { found = found || is_tail(kept_idx); });
                        return found;
                    }
                    tail_parity_.assign(end - begin, 0);
                    for_each_in_clause(sum, [&](const Var kept_idx) { tail_parity_[kept_idx - begin] ^= 1U; });
                    for (std::uint32_t kept_idx = held_pivot_from(sum, 0); kept_idx != none;
                         kept_idx = held_pivot_from(sum, kept_idx + 1)) {
                        const V& held = held_[held_of_pivot_[kept_idx]];
                        steps_ += length(held);
                        for_each_in_clause(held, [&](const Var in_clause) { tail_parity_[in_clause - begin] ^= 1U; });
                    }
                    for (std::uint32_t kept_idx = begin; kept_idx < end; ++kept_idx) {
                        if (tail_parity_[kept_idx - begin] != 0 && is_tail(kept_idx)) {
                            return true;
                        }
                    }
                    return false;
                });
            };
            // The size first, from the clauses in tail groups only: most clauses
            // the cap holds are decided without reading every user.
            std::size_t size = tail.size();
            bool is_over = size > config_.tail_group_cap;
            joined.clear();
            ++mark_stamp_;
            for (const std::uint32_t kept_idx : tail) {
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
                    if (group_mark_[group] == mark_stamp_ || !sums_tail(clause_idx)) {
                        return;
                    }
                    group_mark_[group] = mark_stamp_;
                    joined.push_back(group);
                    size += group_size_[group];
                    is_over = size > config_.tail_group_cap;
                });
            }
            affected.clear();
            if (!is_over) {
                ++mark_stamp_;
                for (const std::uint32_t kept_idx : tail) {
                    for_each_user_beyond_held(kept_idx, [&](std::uint32_t clause_idx) {
                        if (clauses_[clause_idx].present && clause_mark(clause_idx) != mark_stamp_) {
                            clause_mark(clause_idx) = mark_stamp_;
                            if (sums_tail(clause_idx)) {
                                affected.push_back(clause_idx);
                            }
                        }
                    });
                }
            }
            if (is_over) {
                const StoredClause& clause = clauses_[order_[pos]];
                const std::size_t width = clause.rows.size();
                ++counters_.num_held_in_core;
                counters_.num_held_in_core_alone += tail.size() > config_.tail_group_cap ? 1U : 0U;
                counters_.width_held_in_core += width;
                for (const DirectionId direction : clause.row_directions) {
                    hold_in_core(reading_sum_[direction]);
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
            for (const std::uint32_t kept_idx : tail) {
                group_of_kept_[kept_idx] = group;
            }
            for (const std::uint32_t clause_idx : affected) {
                group_of_clause_[clause_idx] = group;
            }
            first_pos = pos;
        }
        if (first_pos < order_.size()) {
            const std::size_t depth = (order_.size() - first_pos) * 1000000 / order_.size();
            counters_.walk_depth_ppm += depth;
            counters_.max_walk_depth_ppm = std::max(counters_.max_walk_depth_ppm, depth);
        }
        for (std::size_t kept_idx = 0; kept_idx < num_kept; ++kept_idx) {
            if (group_of_kept_[kept_idx] == none) {
                counters_.users_of_core_rows += users_[kept_idx].size();
                ++counters_.num_core_rows;
            } else {
                counters_.users_of_tail_rows += users_[kept_idx].size();
            }
        }
    }

    /// @brief The tail groups' rows, the group first reached from the order's end first.
    /// @return Whether anything was eliminated.
    bool eliminate_in_tail_groups() {
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
        bool eliminated = false;
        for (const std::vector<std::uint32_t>& tail_rows : groups) {
            ++counters_.num_tail_groups;
            counters_.num_tail_rows += tail_rows.size();
            ++counters_.tail_groups_of_size[std::min(tail_rows.size(), counters_.tail_groups_of_size.size() - 1)];
            if (found_empty_clause_) {
                continue;
            }
            eliminated = eliminate_in_tail_group(tail_rows) || eliminated;
        }
        return eliminated;
    }

    /// @brief The tail rows of the group in hand that @p direction sums less the
    /// held rows, as bits over the group's rows; computed once per `label_stamp_`.
    std::uint32_t label_of(DirectionId direction) {
        if (label_at_[direction] != label_stamp_) {
            label_at_[direction] = label_stamp_;
            std::uint32_t label = 0;
            beyond_held_in_place(reading_sum_[direction]).for_each_set_bit([&](const std::size_t kept_idx) {
                if (unknown_idx_of_kept_[kept_idx] != none) {
                    label |= std::uint32_t{1} << unknown_idx_of_kept_[kept_idx];
                }
            });
            label_[direction] = label;
        }
        return label_[direction];
    }

    /// @brief Try the choices of one tail group, fewest rows touched first and
    /// equals drawn uniformly, one choice per coset of those accepted, until
    /// none is left; a choice tried is tried again only once the clauses it
    /// touches have changed.
    /// @return Whether anything was eliminated.
    bool eliminate_in_tail_group(const std::vector<std::uint32_t>& tail_rows) {
        const std::size_t size = tail_rows.size();
        for (std::size_t i = 0; i < size; ++i) {
            unknown_idx_of_kept_[tail_rows[i]] = static_cast<std::uint32_t>(i);
        }
        const std::uint32_t num_choices = std::uint32_t{1} << size;
        const std::size_t num_words = (num_choices + 63) / 64;
        // Per label, the choices sharing an odd number of tail rows with it.
        std::unordered_map<std::uint32_t, std::vector<std::uint64_t>> choices_of_label;
        const auto choices_touching = [&](std::uint32_t label) -> const std::vector<std::uint64_t>& {
            auto [found, is_new] = choices_of_label.try_emplace(label);
            if (is_new) {
                found->second.assign(num_words, 0);
                steps_ += num_choices / 64 + 1;
                for (std::uint32_t choice = 1; choice < num_choices; ++choice) {
                    if (odd(label & choice)) {
                        found->second[choice >> 6] |= std::uint64_t{1} << (choice & 63);
                    }
                }
            }
            return found->second;
        };
        struct Leftover {
            std::vector<std::uint64_t> choices; ///< Those touching it.
            std::size_t rows;
        };
        std::unordered_map<std::uint32_t, Leftover> leftovers;
        std::vector<std::size_t> rows_touched(num_choices, 0);
        std::vector<std::uint32_t> clauses_touched(num_choices, 0);
        std::vector<std::uint8_t> is_open(num_choices, 1);
        const auto count = [&](const Leftover& leftover, bool entering) {
            for (std::size_t word_idx = 0; word_idx < num_words; ++word_idx) {
                for (std::uint64_t word = leftover.choices[word_idx]; word != 0; word &= word - 1) {
                    const std::size_t choice = (word_idx << 6) + static_cast<std::size_t>(std::countr_zero(word));
                    rows_touched[choice] = entering ? rows_touched[choice] + leftover.rows
                                                    : rows_touched[choice] - leftover.rows;
                    clauses_touched[choice] = entering ? clauses_touched[choice] + 1 : clauses_touched[choice] - 1;
                    is_open[choice] = 1;
                }
            }
            steps_ += num_choices / 64 + 1;
        };
        const auto enter = [&](std::uint32_t clause_idx) {
            Leftover leftover{.choices = std::vector<std::uint64_t>(num_words, 0),
                              .rows = clauses_[clause_idx].rows.size()};
            for (const DirectionId direction : clauses_[clause_idx].row_directions) {
                const std::uint32_t label = label_of(direction);
                if (label != 0) {
                    plus_or(leftover.choices, choices_touching(label));
                }
            }
            if (std::ranges::any_of(leftover.choices, [](std::uint64_t word) { return word != 0; })) {
                count(leftover, true);
                leftovers.emplace(clause_idx, std::move(leftover));
            }
        };
        const auto enter_every = [&] {
            leftovers.clear();
            std::ranges::fill(rows_touched, 0);
            std::ranges::fill(clauses_touched, 0);
            std::ranges::fill(is_open, 1);
            ++label_stamp_;
            ++mark_stamp_;
            for (const std::uint32_t kept_idx : tail_rows) {
                for_each_user_beyond_held(kept_idx, [&](std::uint32_t clause_idx) {
                    if (clauses_[clause_idx].present && clause_mark(clause_idx) != mark_stamp_) {
                        clause_mark(clause_idx) = mark_stamp_;
                        enter(clause_idx);
                    }
                });
            }
        };
        enter_every();
        // The choices accepted, in reduced echelon form, each led by its lowest bit.
        std::vector<std::uint32_t> accepted;
        std::uint32_t accepted_leads = 0;
        std::vector<std::uint32_t> choices;
        std::vector<std::uint32_t> touched;
        bool eliminated = false;
        while (!found_empty_clause_) {
            // Until an elimination nothing changes: one order serves.
            choices.clear();
            for (std::uint32_t choice = 1; choice < num_choices; ++choice) {
                if ((choice & accepted_leads) == 0 && is_open[choice] != 0 && rows_touched[choice] != 0) {
                    choices.push_back(choice);
                }
            }
            steps_ += num_choices / 64 + choices.size();
            for (std::size_t i = choices.size(); i > 1; --i) {
                std::swap(choices[i - 1], choices[uniform_below(rng_, i)]);
            }
            std::ranges::stable_sort(choices, {}, [&rows_touched](std::uint32_t choice) { return rows_touched[choice]; });
            std::optional<std::uint32_t> taken;
            std::size_t clauses_before = 0;
            for (const std::uint32_t choice : choices) {
                is_open[choice] = 0;
                ++counters_.num_choices_tried;
                if (!is_drawn(clauses_touched[choice])) {
                    ++counters_.num_not_drawn;
                    continue;
                }
                touched.clear();
                for (const auto& [clause_idx, leftover] : leftovers) {
                    if (((leftover.choices[choice >> 6] >> (choice & 63)) & 1U) != 0) {
                        touched.push_back(clause_idx);
                    }
                }
                steps_ += leftovers.size();
                std::ranges::sort(touched);
                clauses_before = clauses_.size();
                if (try_choice(choice, tail_rows, touched)) {
                    taken = choice;
                    break;
                }
            }
            if (!taken.has_value()) {
                break;
            }
            const std::uint32_t choice = *taken;
            ++counters_.num_choices_accepted;
            eliminated = true;
            const std::uint32_t lead = choice & (~choice + 1);
            for (std::uint32_t& row : accepted) {
                if ((row & lead) != 0) {
                    row ^= choice;
                }
            }
            accepted.push_back(choice);
            accepted_leads |= lead;
            const std::size_t units_before = counters_.num_units_substituted;
            substitute_units();
            if (counters_.num_units_substituted != units_before) {
                // A substitution rewrites clauses anywhere.
                enter_every();
                continue;
            }
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
        for (const std::uint32_t kept_idx : tail_rows) {
            unknown_idx_of_kept_[kept_idx] = none;
        }
        return eliminated;
    }

    /// @brief Eliminate by the hyperplane of choice @p choice over @p tail_rows,
    /// touching @p touched, the clauses with a row whose label shares an odd
    /// number of tail rows with it; tried with chance 2^(try_from - n) for n
    /// clauses touched, and not when the same rows of the same clauses were refused.
    /// @return Whether it was accepted.
    bool try_choice(std::uint32_t choice, const std::vector<std::uint32_t>& tail_rows,
                    const std::vector<std::uint32_t>& touched) {
        assert(!touched.empty() && "a choice touches nothing");
        // The same clauses resolved on the same rows were refused before.
        std::string key;
        for (const std::uint32_t clause_idx : touched) {
            key.append(reinterpret_cast<const char*>(&clause_idx), sizeof(clause_idx));
            std::uint8_t bits = 0;
            const std::vector<DirectionId>& rows = clauses_[clause_idx].row_directions;
            for (std::size_t row_idx = 0; row_idx < rows.size(); ++row_idx) {
                bits |= static_cast<std::uint8_t>((odd(label_of(rows[row_idx]) & choice) ? 1U : 0U) << (row_idx & 7));
                if ((row_idx & 7) == 7 || row_idx + 1 == rows.size()) {
                    key.push_back(static_cast<char>(bits));
                    bits = 0;
                }
            }
        }
        steps_ += key.size() / sizeof(std::uint32_t);
        if (refused_keys_.contains(key)) {
            ++counters_.num_choices_refused_before;
            return false;
        }
        std::optional<Resolution> resolution =
            resolve(touched, [this, choice](const StoredClause& clause, std::size_t row_idx) {
                return odd(label_of(clause.row_directions[row_idx]) & choice);
            });
        if (!resolution.has_value()) {
            ++counters_.num_not_accepted;
            refused_keys_.insert(std::move(key));
            return false;
        }
        counters_.num_eliminated_by_gate += resolution->by_gate ? 1U : 0U;
        // The hyperplane holds the held rows: a held pivot changes with the tail
        // rows of its vector the choice changes.
        std::vector<std::uint32_t> changed_kept_idxs;
        std::unordered_map<std::uint32_t, std::uint8_t> held_parity;
        std::vector<std::uint32_t> holding;
        for (std::size_t i = 0; i < tail_rows.size(); ++i) {
            if (((choice >> i) & 1U) != 0) {
                changed_kept_idxs.push_back(tail_rows[i]);
                // A held vector can be listed more than once.
                holding = held_with_[tail_rows[i]];
                std::ranges::sort(holding);
                const auto repeated = std::ranges::unique(holding);
                holding.erase(repeated.begin(), repeated.end());
                for (const std::uint32_t held_idx : holding) {
                    if (held_[held_idx].get_bit(tail_rows[i])) {
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
        replace(touched, std::move(*resolution), reading_flip_set_vars(changed_kept_idxs));
        ++counters_.num_eliminated;
        return true;
    }

    /// @brief The variables of the flip set that changes exactly the kept rows
    /// @p changed_kept_idxs among the reading's kept rows, ascending.
    std::vector<std::uint32_t> reading_flip_set_vars(const std::vector<std::uint32_t>& changed_kept_idxs) {
        if (!has_reading_flip_sets_) {
            has_reading_flip_sets_ = true;
            find_flip_sets_of(adder_of_kept_.size());
        }
        V sum(num_vars_);
        for (const std::uint32_t kept_idx : changed_kept_idxs) {
            plus_assign(sum, flip_set_of_kept_[kept_idx], steps_);
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

    /// @brief Remove each clause some flip set touches alone, retesting the
    /// clauses using the rows of each one removed.
    /// @return Whether one was removed.
    bool eliminate_clauses_touched_alone() {
        // A clause has a flip set to itself only through a kept row it added or
        // one whose adder is gone.
        std::vector<std::uint32_t> waiting;
        std::vector<std::uint8_t> is_waiting(clauses_.size(), 0);
        const auto wait = [&](std::uint32_t clause_idx) {
            if (clauses_[clause_idx].present && is_waiting[clause_idx] == 0) {
                is_waiting[clause_idx] = 1;
                waiting.push_back(clause_idx);
            }
        };
        for (std::size_t pos = 0; pos < order_.size(); ++pos) {
            if (added_begin_[pos] != added_begin_[pos + 1]) {
                wait(order_[pos]);
            }
        }
        for (std::size_t kept_idx = 0; kept_idx < adder_of_kept_.size(); ++kept_idx) {
            if (!clauses_[adder_of_kept_[kept_idx]].present) {
                steps_ += users_[kept_idx].size();
                for (const std::uint32_t user : users_[kept_idx]) {
                    wait(user);
                }
            }
        }
        bool eliminated = false;
        std::vector<std::uint32_t> free_kept_idxs;
        for (std::size_t i = 0; i < waiting.size() && !found_empty_clause_; ++i) {
            const std::uint32_t clause_idx = waiting[i];
            is_waiting[clause_idx] = 0;
            if (!clauses_[clause_idx].present || !eliminate_if_touched_alone(clause_idx, free_kept_idxs)) {
                continue;
            }
            eliminated = true;
            for (const std::uint32_t kept_idx : free_kept_idxs) {
                for (const std::uint32_t user : users_[kept_idx]) {
                    wait(user);
                }
            }
        }
        return eliminated;
    }

    /// @brief Remove clause @p clause_idx if a flip set touches it alone: one
    /// that changes only kept rows the clause added or whose adder is gone.
    /// @param free_kept_idxs Set to those kept rows among the ones its rows sum.
    /// @return Whether it was removed.
    bool eliminate_if_touched_alone(std::uint32_t clause_idx, std::vector<std::uint32_t>& free_kept_idxs) {
        ++counters_.num_clauses_tested_alone;
        const StoredClause& clause = clauses_[clause_idx];
        free_kept_idxs.clear();
        ++mark_stamp_;
        for (const DirectionId direction : clause.row_directions) {
            steps_ += length(reading_sum_[direction]);
            reading_sum_[direction].for_each_set_bit([&](const std::size_t kept_idx) {
                if (kept_mark_[kept_idx] == mark_stamp_) {
                    return;
                }
                kept_mark_[kept_idx] = mark_stamp_;
                const std::uint32_t adder = adder_of_kept_[kept_idx];
                if (adder == clause_idx || !clauses_[adder].present) {
                    free_kept_idxs.push_back(static_cast<std::uint32_t>(kept_idx));
                }
            });
        }
        if (free_kept_idxs.empty()) {
            return false;
        }
        for (std::size_t i = 0; i < free_kept_idxs.size(); ++i) {
            unknown_idx_of_kept_[free_kept_idxs[i]] = static_cast<std::uint32_t>(i);
        }
        const std::size_t num_words = (free_kept_idxs.size() + 63) / 64;
        const auto bits_of = [&](DirectionId direction) {
            UnknownBits bits(num_words, 0);
            reading_sum_[direction].for_each_set_bit([&](const std::size_t kept_idx) {
                if (unknown_idx_of_kept_[kept_idx] != none) {
                    flip(bits, unknown_idx_of_kept_[kept_idx]);
                }
            });
            return bits;
        };
        // The other clauses' rows, in an echelon each led by its lowest bit.
        std::vector<UnknownBits> conditions;
        std::vector<std::size_t> lead_of_condition;
        UnknownBits is_lead(num_words, 0);
        const auto reduce = [&](UnknownBits& bits) {
            for (std::size_t i = 0; i < conditions.size(); ++i) {
                if (test(bits, lead_of_condition[i])) {
                    plus_assign(bits, conditions[i]);
                }
            }
        };
        std::vector<UnknownBits> own;
        own.reserve(clause.row_directions.size());
        for (const DirectionId direction : clause.row_directions) {
            own.push_back(bits_of(direction));
        }
        // The clause's rows reduced against the conditions so far, a condition at a time.
        std::vector<UnknownBits> own_reduced = own;
        // Whether a row of the clause is outside the span of the other clauses' rows so far.
        const auto is_outside = [&] {
            return std::ranges::any_of(own_reduced, [](const UnknownBits& row) { return any(row); });
        };
        const std::size_t clause_stamp = ++mark_stamp_;
        clause_mark(clause_idx) = clause_stamp;
        // Whether the clause is removed, and the flip set drawn, depend only on the span of the other users'
        // rows, so the users are read in turns over the free kept rows, which reaches a full span sooner.
        bool outside = true;
        std::vector<std::size_t> next_user(free_kept_idxs.size(), 0);
        for (bool any_left = true; any_left && outside;) {
            any_left = false;
            for (std::size_t i = 0; i < free_kept_idxs.size() && outside; ++i) {
                const std::vector<std::uint32_t>& users = users_[free_kept_idxs[i]];
                if (next_user[i] == users.size()) {
                    continue;
                }
                any_left = true;
                const std::uint32_t user = users[next_user[i]++];
                ++steps_;
                if (!clauses_[user].present || clause_mark(user) == clause_stamp) {
                    continue;
                }
                clause_mark(user) = clause_stamp;
                bool grew = false;
                for (const DirectionId direction : clauses_[user].row_directions) {
                    UnknownBits condition = bits_of(direction);
                    reduce(condition);
                    if (!any(condition)) {
                        continue;
                    }
                    lead_of_condition.push_back(lowest(condition));
                    flip(is_lead, lead_of_condition.back());
                    for (UnknownBits& row : own_reduced) {
                        if (test(row, lead_of_condition.back())) {
                            plus_assign(row, condition);
                        }
                    }
                    conditions.push_back(std::move(condition));
                    grew = true;
                }
                // Once the clause's rows are in that span, more rows keep them there.
                if (grew && !is_outside()) {
                    outside = false;
                }
            }
        }
        if (!outside) {
            for (const std::uint32_t kept_idx : free_kept_idxs) {
                unknown_idx_of_kept_[kept_idx] = none;
            }
            return false;
        }
        // Uniform among the choices the other rows allow that change a row of the clause.
        UnknownBits changed(num_words, 0);
        do {
            std::ranges::fill(changed, 0);
            for (std::size_t i = 0; i < free_kept_idxs.size(); ++i) {
                if (!test(is_lead, i) && (rng_() & 1U) != 0) {
                    flip(changed, i);
                }
            }
            for (std::size_t i = conditions.size(); i-- > 0;) {
                if (odd_overlap(conditions[i], changed)) {
                    flip(changed, lead_of_condition[i]);
                }
            }
        } while (std::ranges::none_of(own, [&changed](const UnknownBits& row) { return odd_overlap(row, changed); }));
        std::vector<std::uint8_t> row_changes(own.size(), 0);
        std::vector<std::uint32_t> changed_kept_idxs;
        for (std::size_t i = 0; i < own.size(); ++i) {
            row_changes[i] = odd_overlap(own[i], changed) ? 1 : 0;
        }
        for (std::size_t i = 0; i < free_kept_idxs.size(); ++i) {
            if (test(changed, i)) {
                changed_kept_idxs.push_back(free_kept_idxs[i]);
            }
            unknown_idx_of_kept_[free_kept_idxs[i]] = none;
        }
        const std::vector<std::uint32_t> clause_idxs{clause_idx};
        std::optional<Resolution> resolution =
            resolve(clause_idxs, [&row_changes](const StoredClause&, std::size_t row_idx) {
                return row_changes[row_idx] != 0;
            });
        assert(resolution.has_value() && resolution->resolvents.empty() && "one clause resolved to something");
        replace(clause_idxs, std::move(*resolution), reading_flip_set_vars(changed_kept_idxs));
        ++counters_.num_eliminated;
        ++counters_.num_eliminated_in_one_clause;
        return true;
    }

    /// @brief Whether a choice touching @p count clauses is drawn to be tried.
    bool is_drawn(std::size_t count) {
        const double chance = std::exp2(config_.try_from - static_cast<double>(count));
        if (chance >= 1.0) {
            return true;
        }
        return static_cast<double>(rng_() >> 11) * 0x1.0p-53 < chance;
    }

    /// @brief Substitute each unit among the clauses for its lowest variable:
    /// an elimination by the flip set of that variable alone, with the unit as its gate.
    void substitute_units() {
        while (!unit_clause_idxs_.empty() && !found_empty_clause_) {
            const std::uint32_t unit_clause_idx = unit_clause_idxs_.back();
            unit_clause_idxs_.pop_back();
            if (!clauses_[unit_clause_idx].present) {
                continue;
            }
            const Var var = lowest_var(clauses_[unit_clause_idx].rows.front());
            std::vector<std::uint32_t> clause_idxs;
            for (std::uint32_t clause_idx = 0; clause_idx < clauses_.size(); ++clause_idx) {
                const StoredClause& clause = clauses_[clause_idx];
                ++steps_;
                if (clause.present &&
                    std::ranges::any_of(clause.rows, [var](const V& row) { return row.get_bit(var); })) {
                    clause_idxs.push_back(clause_idx);
                }
            }
            std::optional<Resolution> resolution =
                resolve(clause_idxs, [var](const StoredClause& clause, std::size_t row_idx) {
                    return clause.rows[row_idx].get_bit(var);
                });
            assert(resolution.has_value() && resolution->by_gate &&
                   "a substitution left more rows than it removed");
            replace(clause_idxs, std::move(*resolution), {var});
            ++counters_.num_units_substituted;
        }
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
                steps_ += length(row) + steps_per_equation;
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
                    plus_assign(rest, part.changed, steps_);
                }
            }
            assert(found && "the flip set does not touch the clause");
        }
        const std::optional<std::vector<std::uint8_t>> in_gate = find_gate(parts);

        Resolution resolution;
        resolution.by_gate = in_gate.has_value();
        std::unordered_map<std::string, std::size_t> resolvent_idx_of_key;
        std::size_t rows_added = 0;
        // Whether the resolvents still have no more rows than the clauses removed.
        const auto resolve_pair = [&](const std::size_t i, const std::size_t j) {
            ++steps_;
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
                    steps_ += length(row) + steps_per_equation;
                    spanning[spanning_idx++].assign_from(row);
                }
            }
            V& negated_changed = spanning[spanning_idx];
            negated_changed.assign_from(parts[i].changed);
            plus_assign(negated_changed, parts[j].changed, steps_);
            negate(negated_changed);
            if (!canonical_in_place(spanning, leads_, canonical_work_, steps_)) {
                return true;
            }
            const std::vector<std::uint32_t>& by_lead = canonical_work_.by_lead;
            std::string& key = pair_key_;
            key.clear();
            steps_ += by_lead.size() * steps_per_equation;
            for (const std::uint32_t row_idx : by_lead) {
                steps_ += length(spanning[row_idx]);
                append_key(key, spanning[row_idx]);
            }
            if (seen_.contains(key)) {
                return true;
            }
            // By place among the clauses, and not yet by index.
            const std::array<std::uint32_t, 2> parents{static_cast<std::uint32_t>(i),
                                                       static_cast<std::uint32_t>(j)};
            const auto [found, is_new] = resolvent_idx_of_key.try_emplace(key, resolution.resolvents.size());
            if (!is_new) {
                resolution.parents[found->second] =
                    std::min(resolution.parents[found->second], parents);
                return true;
            }
            rows_added += by_lead.size();
            if (rows_added > rows_removed) {
                return false;
            }
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
        in_order.by_gate = resolution.by_gate;
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

    /// @brief The members of a gate among @p parts, shrunk to a minimal one;
    /// nothing if no rests of dimension at most `max_gate_rank` together are
    /// jointly unsatisfiable.
    ///
    /// The rests are added fewest rows first, equal ones in an order drawn uniformly.
    std::optional<std::vector<std::uint8_t>> find_gate(std::span<const SplitClause<V>> parts) {
        std::vector<std::size_t> order(parts.size());
        std::iota(order.begin(), order.end(), std::size_t{0});
        for (std::size_t i = order.size(); i > 1; --i) {
            std::swap(order[i - 1], order[uniform_below(rng_, i)]);
        }
        std::stable_sort(order.begin(), order.end(), [&parts](std::size_t a, std::size_t b) {
            return parts[a].num_rest < parts[b].num_rest;
        });
        steps_ += parts.size();

        GateTable<V>& table = gate_table_;
        table.clear();
        std::vector<std::size_t> members;
        for (const std::size_t i : order) {
            const typename GateTable<V>::Outcome outcome = table.add(parts[i].rests(), steps_);
            if (outcome == GateTable<V>::Outcome::over_rank) {
                continue;
            }
            members.push_back(i);
            if (outcome != GateTable<V>::Outcome::covered) {
                continue;
            }
            const std::vector<std::uint8_t> is_member = table.minimal_cover(steps_);
            std::vector<std::uint8_t> in_gate(parts.size(), 0);
            for (std::size_t added = 0; added < members.size(); ++added) {
                in_gate[members[added]] = is_member[added];
            }
            return in_gate;
        }
        return std::nullopt;
    }

    static constexpr std::uint32_t none = static_cast<std::uint32_t>(-1);

    std::size_t num_vars_;
    BveConfig config_;
    Rng& rng_;
    Leads leads_; ///< Empty between calls of `canonical()`.
    // Scratch of `resolve()` and `find_gate()`.
    std::vector<SplitClause<V>> parts_;
    Rows spanning_;
    CanonicalWork canonical_work_;
    std::string pair_key_;
    GateTable<V> gate_table_;
    std::vector<StoredClause> clauses_; ///< In the order added.
    std::unordered_map<std::string, std::uint32_t> seen_; ///< Key of a present clause to its index.
    std::vector<Elimination> eliminations_;
    BveCounters counters_;
    std::size_t steps_ = 0;
    bool found_empty_clause_ = false;

    std::vector<Direction> directions_;
    std::vector<DirectionId> free_directions_;
    std::unordered_map<std::string, DirectionId> direction_of_key_;
    std::string direction_key_; ///< Scratch.

    // Of a reading.
    std::size_t reading_id_ = 0; ///< Readings start from 1.
    bool is_reading_ = false;
    bool has_reading_flip_sets_ = false;
    /// @brief Per direction, the kept rows it sums in the reading `reading_of_` names.
    std::vector<V> reading_sum_;
    std::vector<std::size_t> reading_of_;
    std::vector<std::uint32_t> order_; ///< The present clauses, in the reading's order.
    std::vector<std::uint32_t> adder_of_kept_; ///< Per kept row, the clause that added it.
    /// @brief The kept rows in echelon form, each row with the kept rows it sums.
    Leads echelon_leads_;
    std::vector<V> echelon_rows_;
    std::vector<V> reading_sums_;
    std::vector<Var> echelon_lead_of_;
    V reduced_; ///< Scratch of `hold_row()`.
    /// @brief Per kept row, the variables of a flip set that changes it and no
    /// other kept row; made at the reading's first elimination.
    std::vector<V> flip_set_of_kept_;
    /// @brief Per place in `order_`, its clause's first kept row; one more at the end.
    std::vector<std::uint32_t> added_begin_;
    /// @brief Per kept row, the clauses with a row summing it, some no longer present.
    std::vector<std::vector<std::uint32_t>> users_;
    std::vector<std::uint32_t> last_user_; ///< Per kept row, the last clause put on its list.
    std::vector<std::uint32_t> group_of_kept_; ///< Per kept row, `none` in the core span.
    std::vector<std::uint32_t> group_of_clause_; ///< Per clause, the tail group it is a leftover of.
    std::vector<std::uint32_t> group_parent_; ///< Union-find over tail groups.
    std::vector<std::uint32_t> group_size_; ///< Of a root, its tail rows.
    std::vector<std::size_t> group_mark_;
    /// @brief The rows of the clauses held in the core span, as vectors of kept
    /// rows in reduced form, each led by its highest kept row.
    std::vector<V> held_;
    std::vector<std::uint32_t> held_pivot_; ///< Per held vector, the kept row leading it.
    IndexSet held_pivots_;
    std::vector<std::uint32_t> held_of_pivot_; ///< Per kept row, the held vector it leads.
    /// @brief Per kept row, held vectors that have it; some no longer do.
    std::vector<std::vector<std::uint32_t>> held_with_;
    V beyond_held_; ///< Scratch of `beyond_held_in_place()`.
    std::vector<std::uint8_t> tail_parity_; ///< Scratch of the walk.
    /// @brief Per kept row, its place among the tail rows or free kept rows in hand; `none` if not one.
    std::vector<std::uint32_t> unknown_idx_of_kept_;
    std::vector<std::size_t> kept_mark_;
    std::vector<std::size_t> clause_mark_;
    std::size_t mark_stamp_ = 0;
    /// @brief Of each choice refused, its clauses and the rows it changes in them.
    std::unordered_set<std::string> refused_keys_;
    std::vector<std::uint32_t> label_; ///< Per direction, see `label_of()`.
    std::vector<std::size_t> label_at_;
    std::size_t label_stamp_ = 0;

    /// @brief The clauses of one row added and not yet substituted; some are removed since.
    std::vector<std::uint32_t> unit_clause_idxs_;
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
    clause.watched0 = 0;
    clause.watched1 = clause.rows() <= 1 ? 0 : 1;
    return clause;
}

template <GF2Vector V>
BvePlan build_bve_plan_over(const std::vector<Clause>& clauses, const ActiveClauseIds active_clause_ids,
                            const std::size_t num_vars, const EquationRepr repr, const BveConfig& config,
                            Rng& rng) {
    using Rows = std::vector<V>;
    const auto start = std::chrono::steady_clock::now();
    BvePlan plan;
    Formula<V> formula(num_vars, config, rng);
    BveCounters& counters = formula.counters();
    for (const ClauseId id : active_clause_ids) {
        assert(clauses[id].num_vars() == num_vars && "an active clause is over another numbering");
        ++counters.clauses_before;
        Rows spanning;
        spanning.reserve(clauses[id].rows());
        for (std::size_t row_idx = 0; row_idx < clauses[id].rows(); ++row_idx) {
            spanning.push_back(gf2_vector_of<V>(clauses[id].row(row_idx)));
        }
        // A clause whose linear negation holds `0 = 1` is a tautology, and goes.
        std::optional<Rows> rows = formula.canonical_of(std::move(spanning));
        if (!rows.has_value()) {
            plan.removed_ids.push_back(id);
            continue;
        }
        counters.equation_bits += equation_bits(*rows);
        formula.add_input_clause(std::move(*rows), id);
    }
    const std::size_t num_input_clauses = formula.clauses().size();
    const IndexSet mentioned_before = formula.mentioned_vars();
    formula.run_projection();
    counters.num_steps = formula.steps();

    plan.found_empty_clause = formula.found_empty_clause();
    plan.eliminations = formula.eliminations();
    plan.dropped_col_idxs = IndexSet(num_vars + 1);
    if (!plan.eliminations.empty()) {
        const IndexSet mentioned_after = formula.mentioned_vars();
        mentioned_before.for_each_set([&](const std::size_t var) {
            if (!mentioned_after.test(var)) {
                plan.dropped_col_idxs.set(var);
            }
        });
    }

    std::size_t num_left = 0;
    for (std::size_t clause_idx = 0; clause_idx < formula.clauses().size(); ++clause_idx) {
        const typename Formula<V>::StoredClause& clause = formula.clauses()[clause_idx];
        if (clause_idx < num_input_clauses) {
            if (clause.present) {
                num_left += 1 + clause.duplicate_ids.size();
            } else {
                plan.removed_ids.push_back(clause.input_id);
                plan.removed_ids.insert(plan.removed_ids.end(), clause.duplicate_ids.begin(),
                                        clause.duplicate_ids.end());
            }
            continue;
        }
        const auto parent = [&](const std::uint32_t parent_clause_idx) {
            return parent_clause_idx < num_input_clauses
                       ? BvePlan::Parent{.is_resolvent = false,
                                         .idx = formula.clauses()[parent_clause_idx].input_id}
                       : BvePlan::Parent{.is_resolvent = true,
                                         .idx = parent_clause_idx - num_input_clauses};
        };
        BvePlan::Resolvent& resolvent = plan.resolvents.emplace_back(BvePlan::Resolvent{
            .parents = {parent(clause.parent_clause_idxs[0]), parent(clause.parent_clause_idxs[1])},
            .clause = std::nullopt,
            .elimination_idx = clause.elimination_idx,
            .removed_clause_idx = clause.removed_clause_idx});
        if (clause.present) {
            ++num_left;
            counters.num_unit_resolvents += clause.rows.size() == 1 ? 1U : 0U;
            counters.num_resolvents_of_width_at_most_2 += clause.rows.size() <= 2 ? 1U : 0U;
            resolvent.clause = clause_of<V>(clause.rows, num_vars, repr);
        }
    }
    std::ranges::sort(plan.removed_ids);
    counters.clauses_after = num_left;

    const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start;
    counters.time_s = elapsed.count();
    plan.counters = counters;
    return plan;
}

} // namespace

BvePlan build_bve_plan(const std::vector<Clause>& clauses,
                       const ActiveClauseIds active_clause_ids, const std::size_t num_vars,
                       const EquationRepr repr, const BveConfig& config, Rng& rng) {
    assert(config.try_from >= 0.0 && "the try constant is out of range");
    assert(config.tail_group_cap >= 1 && config.tail_group_cap <= 16 && config.success_rate >= 0.0 &&
           config.success_rate <= 1.0 && "the tail group cap or the success rate is out of range");
    return with_gf2_vector_of(repr, [&]<GF2Vector V>(std::type_identity<V>) {
        return build_bve_plan_over<V>(clauses, active_clause_ids, num_vars, repr, config, rng);
    });
}

} // namespace xorcle
