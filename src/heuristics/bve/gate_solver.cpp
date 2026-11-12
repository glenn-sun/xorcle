#include "heuristics/bve/gate_solver.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <optional>
#include <utility>
#include <vector>

#include "algebra/dense/dense_gf2_vector.h"
#include "util/index_set.h"
#include "util/random.h"

namespace xorcle {

namespace {

using Row = DenseGF2Vector;
using ClauseIdx = std::uint32_t;
constexpr ClauseIdx none = static_cast<ClauseIdx>(-1);

/// @brief Rows kept in row-echelon form, each led by its lowest bit.
class Echelon {
public:
    explicit Echelon(std::size_t num_bits) : by_lead_(num_bits) {}

    std::size_t rows() const noexcept { return leads_.size(); }

    /// @brief The leading bits of the rows, in the order added.
    const std::vector<std::size_t>& leads() const noexcept { return leads_; }

    const Row& row_led_by(std::size_t lead) const { return *by_lead_[lead]; }

    /// @brief Add @p row to the span; false if it was in it already.
    bool add(Row row) {
        while (true) {
            const std::optional<std::size_t> lead = row.leading_bit();
            if (!lead.has_value()) {
                return false;
            }
            if (!by_lead_[*lead].has_value()) {
                by_lead_[*lead] = std::move(row);
                leads_.push_back(*lead);
                return true;
            }
            row.plus_assign(*by_lead_[*lead]);
        }
    }

    Row take(std::size_t lead) {
        Row row = std::move(*by_lead_[lead]);
        by_lead_[lead].reset();
        leads_.erase(std::find(leads_.begin(), leads_.end(), lead));
        return row;
    }

    std::optional<std::size_t> lowest_lead() const {
        if (leads_.empty()) {
            return std::nullopt;
        }
        return *std::min_element(leads_.begin(), leads_.end());
    }

private:
    std::vector<std::optional<Row>> by_lead_;
    std::vector<std::size_t> leads_;
};

/// @brief A basis of the intersection of the spans of @p left and @p right, by
/// Zassenhaus over both halves of rows twice as long.
/// @pre The rows of @p left are independent.
std::vector<Row> intersection_of(const std::vector<Row>& left, const std::vector<Row>& right,
                                 std::size_t num_bits) {
    Echelon doubled(2 * num_bits);
    for (const Row& row : left) {
        Row both(2 * num_bits);
        row.for_each_set_bit([&](std::size_t bit) {
            both.set_bit(bit);
            both.set_bit(num_bits + bit);
        });
        doubled.add(std::move(both));
    }
    for (const Row& row : right) {
        Row first(2 * num_bits);
        row.for_each_set_bit([&](std::size_t bit) { first.set_bit(bit); });
        doubled.add(std::move(first));
    }
    std::vector<Row> basis;
    for (const std::size_t lead : doubled.leads()) {
        if (lead < num_bits) {
            continue;
        }
        Row& second = basis.emplace_back(num_bits);
        doubled.row_led_by(lead).for_each_set_bit(
            [&](std::size_t bit) { second.set_bit(bit - num_bits); });
    }
    return basis;
}

struct SolverClause {
    std::vector<Row> rows;
    /// @brief Rows independent modulo the trail when last visited, or one whose
    /// negation the trail holds.
    std::array<std::uint32_t, 2> tracked{0, 0};
    /// @brief Of a learned clause, the input clauses it derives from.
    std::vector<std::uint64_t> antecedents;
    /// @brief Of a learned clause, the clauses it was resolved from, in replay order.
    std::vector<ClauseIdx> reasons;
    ClauseIdx prev = none; ///< In move-to-front order.
    ClauseIdx next = none;
};

struct TrailEntry {
    Row equation; ///< Reduced by the entries before it.
    std::size_t lead;
    ClauseIdx reason; ///< `none` for a decision.
    std::size_t level;
};

enum class Visit { quiet, propagated, conflict };

struct SolveResult {
    std::optional<std::vector<ClauseIdx>> core;
    GateRefutation refutation;
};

class GateSolver {
public:
    GateSolver(const std::vector<std::vector<Row>>& clauses, std::size_t num_bits,
               std::size_t max_conflicts, Rng& rng)
        : num_bits_(num_bits),
          num_inputs_(clauses.size()),
          max_conflicts_(max_conflicts),
          rng_(rng),
          first_(num_bits),
          second_(num_bits) {
        clauses_.reserve(clauses.size());
        for (const std::vector<Row>& rows : clauses) {
            clauses_.emplace_back().rows = rows;
        }
    }


    /// @brief The core of @p input_idxs, ascending, if they are unsatisfiable
    /// together and the solve stays under the cap.
    SolveResult solve(const std::vector<ClauseIdx>& input_idxs) {
        clauses_.resize(num_inputs_, SolverClause{});
        trail_.clear();
        level_ = 0;
        scan_ = input_idxs;
        shuffle_uniformly(scan_, rng_);
        front_ = none;
        for (auto i = scan_.rbegin(); i != scan_.rend(); ++i) {
            SolverClause& clause = clauses_[*i];
            clause.tracked = {0, clause.rows.size() >= 2 ? 1U : 0U};
            clause.prev = none;
            clause.next = none;
            push_front(*i);
        }
        cursor_ = 0;
        priority_.clear();
        std::size_t num_conflicts = 0;
        while (true) {
            const ClauseIdx conflicting = propagate();
            if (conflicting != none) {
                ++num_conflicts;
                if (level_ == 0) {
                    return root_core(conflicting);
                }
                if (num_conflicts == max_conflicts_) {
                    return {.core = std::nullopt, .refutation = {}};
                }
                learn(conflicting);
                continue;
            }
            if (!decide()) {
                return {.core = std::nullopt, .refutation = {}};
            }
        }
    }

private:
    Row false_row() const {
        Row row(num_bits_);
        row.set_bit(num_bits_ - 1);
        return row;
    }

    bool is_false(const Row& row) const {
        return row.leading_bit() == std::optional<std::size_t>(num_bits_ - 1);
    }

    static bool is_zero(const Row& row) { return !row.leading_bit().has_value(); }

    void reduce(Row& row) {
        for (const TrailEntry& entry : trail_) {
            if (row.get_bit(entry.lead)) {
                row.plus_assign(entry.equation);
            }
        }
    }

    /// @brief @p row in the trail basis: bit 0 is `0 = 1`, bit `m - i` trail entry `i`.
    /// @pre @p row is in the span of the trail and `0 = 1`.
    Row in_trail_basis(const Row& row) {
        const std::size_t m = trail_.size();
        Row coordinates(m + 1);
        Row rest = row;
        for (std::size_t i = 0; i < m; ++i) {
            if (rest.get_bit(trail_[i].lead)) {
                rest.plus_assign(trail_[i].equation);
                coordinates.set_bit(m - i);
            }
        }
        if (!is_zero(rest)) {
            assert(is_false(rest) && "a reason row is outside the trail's span with 0 = 1");
            coordinates.set_bit(0);
        }
        return coordinates;
    }

    void push(Row equation, ClauseIdx reason) {
        const std::optional<std::size_t> lead = equation.leading_bit();
        assert(lead.has_value() && *lead != num_bits_ - 1 && "a trail equation is 0 = 0 or 0 = 1");
        trail_.push_back(TrailEntry{.equation = std::move(equation), .lead = *lead,
                                    .reason = reason, .level = level_});
    }

    // --- Move-to-front order ----------------------------------------------------

    void push_front(ClauseIdx idx) {
        SolverClause& clause = clauses_[idx];
        clause.prev = none;
        clause.next = front_;
        if (front_ != none) {
            clauses_[front_].prev = idx;
        }
        front_ = idx;
    }

    void move_to_front(ClauseIdx idx) {
        if (front_ == idx) {
            return;
        }
        SolverClause& clause = clauses_[idx];
        clauses_[clause.prev].next = clause.next;
        if (clause.next != none) {
            clauses_[clause.next].prev = clause.prev;
        }
        push_front(idx);
    }

    // --- Propagation ------------------------------------------------------------

    /// @return The conflicting clause, or `none` at a fixed point.
    ClauseIdx propagate() {
        std::size_t priority_idx = 0;
        std::size_t quiet_in_a_row = 0;
        while (true) {
            ClauseIdx idx;
            if (priority_idx < priority_.size()) {
                idx = priority_[priority_idx++];
            } else {
                if (quiet_in_a_row >= scan_.size()) {
                    priority_.clear();
                    return none;
                }
                if (cursor_ >= scan_.size()) {
                    cursor_ = 0;
                }
                idx = scan_[cursor_++];
                ++quiet_in_a_row;
            }
            const Visit visit = visit_clause(idx);
            if (visit == Visit::conflict) {
                priority_.clear();
                return idx;
            }
            if (visit == Visit::propagated) {
                quiet_in_a_row = 0;
            }
        }
    }

    Visit visit_clause(ClauseIdx idx) {
        SolverClause& clause = clauses_[idx];
        if (clause.rows.size() >= 2) {
            first_.assign_from(clause.rows[clause.tracked[0]]);
            reduce(first_);
            if (is_false(first_)) {
                return Visit::quiet;
            }
            second_.assign_from(clause.rows[clause.tracked[1]]);
            reduce(second_);
            if (is_false(second_)) {
                return Visit::quiet;
            }
            if (!is_zero(first_) && !is_zero(second_) && !first_.equals(second_)) {
                return Visit::quiet;
            }
        }
        return visit_whole_clause(idx);
    }

    /// @brief Reduce every row: two distinct nonzero residues are independent, and
    /// otherwise the span modulo the trail is one residue or nothing.
    Visit visit_whole_clause(ClauseIdx idx) {
        SolverClause& clause = clauses_[idx];
        const std::size_t width = clause.rows.size();
        while (residues_.size() < width) {
            residues_.emplace_back(num_bits_);
        }
        std::optional<std::uint32_t> nonzero;
        std::optional<std::uint32_t> other;
        for (std::uint32_t row_idx = 0; row_idx < width; ++row_idx) {
            Row& residue = residues_[row_idx];
            residue.assign_from(clause.rows[row_idx]);
            reduce(residue);
            if (is_false(residue)) {
                clause.tracked = {row_idx, row_idx};
                return Visit::quiet;
            }
            if (is_zero(residue)) {
                continue;
            }
            if (!nonzero.has_value()) {
                nonzero = row_idx;
            } else if (!other.has_value() && !residue.equals(residues_[*nonzero])) {
                other = row_idx;
            }
        }
        if (!nonzero.has_value()) {
            return Visit::conflict;
        }
        if (other.has_value()) {
            clause.tracked = {*nonzero, *other};
            return Visit::quiet;
        }
        Row propagated = residues_[*nonzero];
        propagated.flip_bit(num_bits_ - 1);
        push(std::move(propagated), idx);
        clause.tracked = {*nonzero, *nonzero};
        return Visit::propagated;
    }

    // --- Decisions --------------------------------------------------------------

    /// @brief The span modulo the trail of an open clause; nothing if the trail
    /// satisfies it.
    std::optional<std::vector<Row>> open_span(ClauseIdx idx) {
        const SolverClause& clause = clauses_[idx];
        if (!clause.rows.empty()) {
            first_.assign_from(clause.rows[clause.tracked[0]]);
            reduce(first_);
            if (is_false(first_)) {
                return std::nullopt;
            }
        }
        Echelon span(num_bits_);
        for (const Row& row : clause.rows) {
            Row residue = row;
            reduce(residue);
            span.add(std::move(residue));
        }
        std::vector<Row> rows;
        for (const std::size_t lead : span.leads()) {
            if (lead == num_bits_ - 1) {
                return std::nullopt;
            }
            rows.push_back(span.row_led_by(lead));
        }
        assert(rows.size() >= 2 && "a clause is unit or conflicting at a fixed point");
        return rows;
    }

    /// @return False if every clause is satisfied.
    bool decide() {
        std::vector<Row> intersection;
        std::vector<ClauseIdx> intersected;
        for (ClauseIdx idx = front_; idx != none; idx = clauses_[idx].next) {
            std::optional<std::vector<Row>> span = open_span(idx);
            if (!span.has_value()) {
                continue;
            }
            if (intersected.empty()) {
                intersection = std::move(*span);
                intersected.push_back(idx);
                continue;
            }
            std::vector<Row> narrowed = intersection_of(intersection, *span, num_bits_);
            if (narrowed.empty()) {
                continue;
            }
            intersection = std::move(narrowed);
            intersected.push_back(idx);
        }
        if (intersected.empty()) {
            return false;
        }
        Row decision(num_bits_);
        random_nonzero_coefficients(intersection.size(), rng_).for_each_set(
            [&](std::size_t row_idx) { decision.plus_assign(intersection[row_idx]); });
        ++level_;
        push(std::move(decision), none);
        priority_ = std::move(intersected);
        return true;
    }

    // --- Conflict analysis ------------------------------------------------------

    void add_antecedents(std::vector<std::uint64_t>& antecedents, ClauseIdx idx) const {
        if (idx < num_inputs_) {
            antecedents[idx >> 6] |= std::uint64_t{1} << (idx & 63);
            return;
        }
        const std::vector<std::uint64_t>& of_clause = clauses_[idx].antecedents;
        for (std::size_t word_idx = 0; word_idx < antecedents.size(); ++word_idx) {
            antecedents[word_idx] |= of_clause[word_idx];
        }
    }

    /// @brief Resolve the first row of @p clause, in the trail basis, with the
    /// reason of the trail entry leading it.
    /// @return The reason.
    ClauseIdx resolve_first_row(Echelon& clause) {
        const std::size_t m = trail_.size();
        const std::size_t lead = *clause.lowest_lead();
        const TrailEntry& entry = trail_[m - lead];
        assert(entry.reason != none && "the resolution walk reached a decision");
        Echelon reason(m + 1);
        for (const Row& row : clauses_[entry.reason].rows) {
            reason.add(in_trail_basis(row));
        }
        assert(reason.lowest_lead() == std::optional<std::size_t>(0) &&
               "a reason clause does not hold its propagated equation's negation");
        Row resolvent = clause.take(lead);
        Row reason_first = reason.take(0);
        resolvent.plus_assign(reason_first);
        resolvent.flip_bit(0);
        assert(!resolvent.get_bit(lead) && !resolvent.get_bit(0) && "a resolution step kept its lead");
        clause.add(std::move(resolvent));
        for (const std::size_t reason_lead : std::vector<std::size_t>(reason.leads())) {
            clause.add(reason.take(reason_lead));
        }
        return entry.reason;
    }

    /// @brief The input clauses the empty clause derives from at the root, and the
    /// derivation through the learned clauses it needs.
    SolveResult root_core(ClauseIdx conflicting) {
        std::vector<std::uint64_t> antecedents((num_inputs_ + 63) / 64, 0);
        add_antecedents(antecedents, conflicting);
        std::vector<ClauseIdx> used{conflicting};
        Echelon clause(trail_.size() + 1);
        for (const Row& row : clauses_[conflicting].rows) {
            clause.add(in_trail_basis(row));
        }
        while (clause.rows() > 0) {
            const ClauseIdx reason = resolve_first_row(clause);
            add_antecedents(antecedents, reason);
            used.push_back(reason);
        }
        std::vector<ClauseIdx> core;
        for (ClauseIdx idx = 0; idx < num_inputs_; ++idx) {
            if (((antecedents[idx >> 6] >> (idx & 63)) & 1U) != 0) {
                core.push_back(idx);
            }
        }
        std::ranges::reverse(used);
        return {.core = std::move(core), .refutation = refutation_of(used)};
    }

    /// @brief The lemmas the empty clause resolved from @p reasons needs, in the order learned.
    GateRefutation refutation_of(const std::vector<ClauseIdx>& reasons) const {
        std::vector<std::uint8_t> needed(clauses_.size(), 0);
        std::vector<ClauseIdx> stack(reasons);
        while (!stack.empty()) {
            const ClauseIdx idx = stack.back();
            stack.pop_back();
            if (idx < num_inputs_ || needed[idx] != 0) {
                continue;
            }
            needed[idx] = 1;
            stack.insert(stack.end(), clauses_[idx].reasons.begin(), clauses_[idx].reasons.end());
        }
        std::vector<std::uint32_t> place(clauses_.size(), 0);
        for (ClauseIdx idx = 0; idx < num_inputs_; ++idx) {
            place[idx] = idx;
        }
        GateRefutation refutation;
        const auto places_of = [&](const std::vector<ClauseIdx>& idxs) {
            std::vector<std::uint32_t> places;
            places.reserve(idxs.size());
            for (const ClauseIdx idx : idxs) {
                places.push_back(place[idx]);
            }
            return places;
        };
        for (auto idx = static_cast<ClauseIdx>(num_inputs_); idx < clauses_.size(); ++idx) {
            if (needed[idx] == 0) {
                continue;
            }
            place[idx] = static_cast<std::uint32_t>(num_inputs_ + refutation.lemmas.size());
            refutation.lemmas.push_back({.rows = clauses_[idx].rows, .reasons = places_of(clauses_[idx].reasons)});
        }
        refutation.reasons = places_of(reasons);
        return refutation;
    }

    /// @brief Learn the asserting clause, backjump to its asserting level and have
    /// the selector visit it first.
    void learn(ClauseIdx conflicting) {
        const std::size_t m = trail_.size();
        const auto level_of_lead = [&](std::size_t lead) { return trail_[m - lead].level; };
        SolverClause learned;
        learned.antecedents.assign((num_inputs_ + 63) / 64, 0);
        add_antecedents(learned.antecedents, conflicting);
        std::vector<ClauseIdx> used{conflicting};
        Echelon clause(m + 1);
        for (const Row& row : clauses_[conflicting].rows) {
            clause.add(in_trail_basis(row));
        }
        const auto at_this_level = [&] {
            return std::count_if(clause.leads().begin(), clause.leads().end(),
                                 [&](std::size_t lead) { return level_of_lead(lead) == level_; });
        };
        while (at_this_level() >= 2) {
            const ClauseIdx reason = resolve_first_row(clause);
            add_antecedents(learned.antecedents, reason);
            used.push_back(reason);
        }
        assert(at_this_level() == 1 && "a conflict does not reach the current level");
        const std::size_t first_lead = *clause.lowest_lead();
        std::size_t asserting_level = 0;
        for (const std::size_t lead : clause.leads()) {
            if (lead != first_lead) {
                asserting_level = std::max(asserting_level, level_of_lead(lead));
            }
        }
        for (const std::size_t lead : clause.leads()) {
            Row& equation = learned.rows.emplace_back(num_bits_);
            clause.row_led_by(lead).for_each_set_bit(
                [&](std::size_t bit) { equation.plus_assign(trail_[m - bit].equation); });
        }

        while (!trail_.empty() && trail_.back().level > asserting_level) {
            trail_.pop_back();
        }
        level_ = asserting_level;
        for (const ClauseIdx idx : used) {
            move_to_front(idx);
        }
        learned.reasons.assign(used.rbegin(), used.rend());
        learned.tracked = {0, learned.rows.size() >= 2 ? 1U : 0U};
        const auto idx = static_cast<ClauseIdx>(clauses_.size());
        clauses_.push_back(std::move(learned));
        push_front(idx);
        scan_.push_back(idx);
        priority_.assign(1, idx);
    }

    std::size_t num_bits_;
    std::size_t num_inputs_;
    std::size_t max_conflicts_; ///< Per solve.
    Rng& rng_;
    std::vector<SolverClause> clauses_; ///< The inputs, then the clauses learned in this solve.
    std::vector<ClauseIdx> scan_; ///< The cyclic selector's order.
    std::size_t cursor_ = 0;
    std::vector<ClauseIdx> priority_; ///< Visited before the selector resumes.
    ClauseIdx front_ = none;
    std::vector<TrailEntry> trail_;
    std::size_t level_ = 0;
    Row first_;
    Row second_;
    std::vector<Row> residues_;
};

} // namespace

GateSearch search_gate(const std::vector<std::vector<DenseGF2Vector>>& clauses,
                       const std::size_t num_bits, const std::size_t max_conflicts, Rng& rng) {
    assert(num_bits >= 1 && max_conflicts >= 1 && "a gate search over no bits or with no conflicts");
    GateSolver solver(clauses, num_bits, max_conflicts, rng);
    GateSearch search;
    std::vector<ClauseIdx> all(clauses.size());
    for (ClauseIdx idx = 0; idx < all.size(); ++idx) {
        all[idx] = idx;
    }
    SolveResult first = solver.solve(all);
    if (!first.core.has_value()) {
        return search;
    }
    search.core = std::move(*first.core);
    search.refutation = std::move(first.refutation);
    return search;
}

namespace {

/// @brief Word @p word_idx of the truth table of coordinate @p coordinate.
std::uint64_t coordinate_word(std::size_t coordinate, std::size_t word_idx) {
    constexpr std::array<std::uint64_t, 6> in_word{
        0xaaaaaaaaaaaaaaaaULL, 0xccccccccccccccccULL, 0xf0f0f0f0f0f0f0f0ULL,
        0xff00ff00ff00ff00ULL, 0xffff0000ffff0000ULL, 0xffffffff00000000ULL};
    if (coordinate < 6) {
        return in_word[coordinate];
    }
    return ((word_idx >> (coordinate - 6)) & 1U) != 0 ? ~std::uint64_t{0} : 0;
}

} // namespace

bool GateTable::add(const std::vector<DenseGF2Vector>& clause) {
    // Each row as the points of one word at which it holds, and the coordinates that
    // flip that from word to word.
    struct Split {
        std::uint64_t in_word;
        std::uint64_t of_word;
    };
    std::vector<Split> split;
    split.reserve(clause.size());
    for (const Row& row : clause) {
        Split& part = split.emplace_back(Split{.in_word = ~std::uint64_t{0}, .of_word = 0});
        row.for_each_set_bit([&](const std::size_t bit) {
            if (bit < 6 && bit < num_coordinates_) {
                part.in_word ^= coordinate_word(bit, 0);
            } else if (bit < num_coordinates_) {
                part.of_word |= std::uint64_t{1} << (bit - 6);
            } else {
                part.in_word = ~part.in_word;
            }
        });
    }
    const std::size_t offset = forbidden_by_.size();
    forbidden_by_.resize(offset + num_words_);
    bool covered = true;
    for (std::size_t word_idx = 0; word_idx < num_words_; ++word_idx) {
        std::uint64_t forbidden = ~std::uint64_t{0};
        for (const Split& part : split) {
            const bool flips = (std::popcount(part.of_word & word_idx) & 1) != 0;
            forbidden &= flips ? ~part.in_word : part.in_word;
        }
        forbidden_by_[offset + word_idx] = forbidden;
        forbidden_[word_idx] |= forbidden;
        covered = covered && forbidden_[word_idx] == ~std::uint64_t{0};
    }
    ++num_added_;
    return covered;
}

std::vector<std::uint8_t> GateTable::minimal_cover() const {
    std::vector<std::uint8_t> is_member(num_added_, 1);
    std::size_t num_members = num_added_;
    for (std::size_t added = 0; added < num_added_ && num_members > 1; ++added) {
        bool others_cover = true;
        for (std::size_t word_idx = 0; word_idx < num_words_ && others_cover; ++word_idx) {
            std::uint64_t forbidden = 0;
            for (std::size_t other = 0; other < num_added_; ++other) {
                if (other != added && is_member[other] != 0) {
                    forbidden |= forbidden_by_[(other * num_words_) + word_idx];
                }
            }
            others_cover = forbidden == ~std::uint64_t{0};
        }
        if (others_cover) {
            is_member[added] = 0;
            --num_members;
        }
    }
    return is_member;
}

GateSearch GateTable::search(const std::vector<std::vector<DenseGF2Vector>>& clauses,
                             const std::size_t num_bits, Rng& rng) {
    assert(num_bits >= 1 && num_bits <= 70 && "a truth table over no bits or past 2^64 words");
    num_coordinates_ = num_bits - 1;
    // One word holds six coordinates, and a table over fewer repeats.
    num_words_ = std::size_t{1} << (std::max<std::size_t>(num_coordinates_, 6) - 6);
    num_added_ = 0;
    forbidden_.assign(num_words_, 0);
    forbidden_by_.clear();

    std::vector<ClauseIdx> order(clauses.size());
    std::iota(order.begin(), order.end(), ClauseIdx{0});
    shuffle_uniformly(order, rng);
    std::stable_sort(order.begin(), order.end(), [&clauses](ClauseIdx a, ClauseIdx b) {
        return clauses[a].size() < clauses[b].size();
    });
    GateSearch search;
    search.by_table = true;
    for (std::size_t added = 0; added < order.size(); ++added) {
        if (!add(clauses[order[added]])) {
            continue;
        }
        const std::vector<std::uint8_t> is_member = minimal_cover();
        std::vector<ClauseIdx> core;
        for (std::size_t member = 0; member <= added; ++member) {
            if (is_member[member] != 0) {
                core.push_back(order[member]);
            }
        }
        std::ranges::sort(core);
        search.core = std::move(core);
        break;
    }
    return search;
}

GateSearch search_gate_hybrid(const std::vector<std::vector<DenseGF2Vector>>& clauses,
                              const std::size_t num_bits, const std::size_t max_table_cost,
                              const std::size_t max_conflicts, GateTable& table, Rng& rng) {
    assert(num_bits >= 1 && max_conflicts >= 1 && "a gate search over no bits or with no conflicts");
    const std::size_t rank = num_bits - 1;
    const std::size_t num_clauses = clauses.size();
    bool by_table = false;
    if (rank < 70) {
        const std::size_t num_words = std::size_t{1} << (std::max<std::size_t>(rank, 6) - 6);
        // On integers, c² × w <= B exactly when c <= (B / w) / c.
        by_table = num_words <= max_table_cost &&
                   (num_clauses == 0 || num_clauses <= max_table_cost / num_words / num_clauses);
    }
    if (by_table) {
        return table.search(clauses, num_bits, rng);
    }
    return search_gate(clauses, num_bits, max_conflicts, rng);
}

GateRefutation refute_gate(const std::vector<std::vector<DenseGF2Vector>>& clauses,
                           const std::size_t num_bits, const std::vector<std::uint32_t>& gate, Rng& rng) {
    assert(std::ranges::is_sorted(gate) && !gate.empty() && gate.back() < clauses.size() &&
           "a gate is not ascending among the clauses");
    std::vector<std::vector<Row>> gate_clauses;
    gate_clauses.reserve(gate.size());
    for (const ClauseIdx idx : gate) {
        gate_clauses.push_back(clauses[idx]);
    }
    GateSearch search = search_gate(gate_clauses, num_bits, std::numeric_limits<std::size_t>::max(), rng);
    assert(search.core.has_value() && "a gate is satisfiable");
    // From places among the gate's clauses to places among all of them.
    const auto remap = [&](std::vector<std::uint32_t>& reasons) {
        for (std::uint32_t& reason : reasons) {
            reason = reason < gate.size()
                         ? gate[reason]
                         : static_cast<std::uint32_t>(reason - gate.size() + clauses.size());
        }
    };
    for (GateRefutation::Lemma& lemma : search.refutation.lemmas) {
        remap(lemma.reasons);
    }
    remap(search.refutation.reasons);
    return std::move(search.refutation);
}

} // namespace xorcle
