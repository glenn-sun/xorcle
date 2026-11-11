#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <optional>
#include <utility>
#include <vector>

#include "algebra/dense/dense_gf2_vector.h"
#include "algebra/equation.h"
#include "algebra/equation_repr.h"
#include "algebra/row_echelon_matrix.h"
#include "heuristics/bve/bve.h"
#include "heuristics/bve/gate_solver.h"
#include "util/random.h"

namespace xorcle {

namespace {

using Row = DenseGF2Vector;
using Rows = std::vector<Row>;

/// @brief Whether some row of @p clause fails at @p point, the assignment whose
/// bit `i` is coordinate `i`.
bool satisfies(const Rows& clause, std::uint32_t point, std::size_t num_coordinates) {
    return std::ranges::any_of(clause, [&](const Row& row) {
        bool sum = row.get_bit(num_coordinates);
        for (std::size_t i = 0; i < num_coordinates; ++i) {
            sum = sum != (row.get_bit(i) && ((point >> i) & 1U) != 0);
        }
        return sum;
    });
}

bool satisfiable(const std::vector<Rows>& clauses, const std::vector<std::uint32_t>& members,
                 std::size_t num_coordinates) {
    for (std::uint32_t point = 0; point < (std::uint32_t{1} << num_coordinates); ++point) {
        if (std::ranges::all_of(members, [&](std::uint32_t idx) {
                return satisfies(clauses[idx], point, num_coordinates);
            })) {
            return true;
        }
    }
    return false;
}

/// @brief From 1 to @p max_width independent random rows over @p num_bits bits whose
/// span does not hold `0 = 1`.
Rows random_clause(std::size_t num_bits, std::size_t max_width, Rng& rng) {
    Rows rows;
    // An echelon of the span, led by the lowest bit, to test each new row against.
    std::vector<Row> echelon;
    const std::size_t width = 1 + uniform_below(rng, max_width);
    for (std::size_t attempt = 0; attempt < 4 * max_width && rows.size() < width; ++attempt) {
        Row row(num_bits);
        for (std::size_t bit = 0; bit < num_bits; ++bit) {
            if (uniform_below(rng, 3) == 0) {
                row.set_bit(bit);
            }
        }
        Row reduced = row;
        for (const Row& other : echelon) {
            if (reduced.get_bit(*other.leading_bit())) {
                reduced.plus_assign(other);
            }
        }
        const std::optional<std::size_t> lead = reduced.leading_bit();
        if (!lead.has_value() || *lead == num_bits - 1) {
            continue;
        }
        for (Row& other : echelon) {
            if (other.get_bit(*lead)) {
                other.plus_assign(reduced);
            }
        }
        echelon.push_back(std::move(reduced));
        rows.push_back(std::move(row));
    }
    return rows;
}

/// @brief Whether unit propagation through @p reasons in their order, under the span
/// of @p assumed, propagates something new at each and ends in a conflict, as the
/// LRUP(⊕) checker requires of a line.
bool replays(const Rows& assumed, const std::vector<const Rows*>& reasons, std::size_t num_bits) {
    std::vector<Row> echelon; // Each row led by its lowest bit, reduced by those before it.
    const auto reduce = [&](Row row) {
        for (const Row& other : echelon) {
            if (row.get_bit(*other.leading_bit())) {
                row.plus_assign(other);
            }
        }
        return row;
    };
    const auto add = [&](const Row& row) {
        Row reduced = reduce(row);
        if (reduced.leading_bit().has_value()) {
            echelon.push_back(std::move(reduced));
        }
    };
    for (const Row& row : assumed) {
        add(row);
    }
    for (const Rows* reason : reasons) {
        std::vector<Row> residues;
        for (const Row& row : *reason) {
            Row residue = reduce(row);
            if (residue.leading_bit().has_value() &&
                std::ranges::none_of(residues, [&](const Row& other) { return other.equals(residue); })) {
                residues.push_back(std::move(residue));
            }
        }
        if (residues.empty()) {
            return true;
        }
        if (residues.size() > 1) {
            return false;
        }
        Row propagated = residues[0];
        propagated.flip_bit(num_bits - 1);
        if (!reduce(propagated).leading_bit().has_value()) {
            return false;
        }
        add(propagated);
    }
    return false;
}

/// @brief Whether every line of @p refutation of @p clauses replays, with @p extra
/// added to each line's linear negation and to each clause's.
bool refutation_replays(const std::vector<Rows>& clauses, const GateRefutation& refutation,
                        const Rows& extra, std::size_t num_bits) {
    std::vector<Rows> lines;
    for (const Rows& clause : clauses) {
        Rows& line = lines.emplace_back(extra);
        line.insert(line.end(), clause.begin(), clause.end());
    }
    const auto cited = [&](const std::vector<std::uint32_t>& reasons) {
        std::vector<const Rows*> cited_lines;
        for (const std::uint32_t reason : reasons) {
            cited_lines.push_back(&lines.at(reason));
        }
        return cited_lines;
    };
    for (const GateRefutation::Lemma& lemma : refutation.lemmas) {
        Rows line = extra;
        line.insert(line.end(), lemma.rows.begin(), lemma.rows.end());
        if (!replays(line, cited(lemma.reasons), num_bits)) {
            return false;
        }
        lines.push_back(std::move(line));
    }
    return replays(extra, cited(refutation.reasons), num_bits);
}

/// @brief @p rows over one more coordinate, inserted before the constant bit.
Rows widened(const Rows& rows, std::size_t num_bits) {
    Rows wide;
    for (const Row& row : rows) {
        Row& wide_row = wide.emplace_back(num_bits + 1);
        row.for_each_set_bit([&](std::size_t bit) { wide_row.set_bit(bit == num_bits - 1 ? num_bits : bit); });
    }
    return wide;
}

/// @brief Whether, lifted by a new coordinate x with `x = 0` taken into the linear
/// negation of every clause and line, @p core makes `x = 0` false and @p refutation
/// replays to its negation.
bool lifted_refutation_replays(const std::vector<Rows>& clauses, const std::vector<std::uint32_t>& core,
                               const GateRefutation& refutation, std::size_t num_coordinates) {
    const std::size_t num_wide_bits = num_coordinates + 2;
    Row x(num_wide_bits);
    x.set_bit(num_coordinates);
    std::vector<Rows> wide_clauses;
    std::vector<Rows> lifted;
    for (const Rows& clause : clauses) {
        wide_clauses.push_back(widened(clause, num_coordinates + 1));
        Rows& lifted_clause = lifted.emplace_back(Rows{x});
        lifted_clause.insert(lifted_clause.end(), wide_clauses.back().begin(), wide_clauses.back().end());
    }
    GateRefutation wide_refutation = refutation;
    for (GateRefutation::Lemma& lemma : wide_refutation.lemmas) {
        lemma.rows = widened(lemma.rows, num_coordinates + 1);
    }
    for (std::uint32_t point = 0; point < (std::uint32_t{1} << (num_coordinates + 1)); ++point) {
        const bool in_core = std::ranges::all_of(core, [&](std::uint32_t idx) {
            return satisfies(lifted[idx], point, num_coordinates + 1);
        });
        if (in_core && ((point >> num_coordinates) & 1U) == 0) {
            return false;
        }
    }
    return refutation_replays(wide_clauses, wide_refutation, {x}, num_wide_bits);
}

} // namespace

TEST_CASE("gate solver: verdicts and cores agree with brute force, under the conflict cap too") {
    Rng rng = make_rng(default_seed, RngComponent::bve_draw);
    std::size_t num_unsat = 0;
    std::size_t num_sat = 0;
    std::size_t num_shrunk = 0;
    std::size_t num_capped = 0;
    std::size_t num_finished_under_small_cap = 0;
    for (std::size_t round = 0; round < 3000; ++round) {
        const std::size_t num_coordinates = 1 + uniform_below(rng, 10);
        const std::size_t num_clauses = 1 + uniform_below(rng, 3 * num_coordinates + 4);
        const std::size_t max_width = 1 + uniform_below(rng, std::min<std::size_t>(num_coordinates, 6));
        std::vector<Rows> clauses;
        for (std::size_t i = 0; i < num_clauses; ++i) {
            clauses.push_back(random_clause(num_coordinates + 1, max_width, rng));
        }
        std::vector<std::uint32_t> all(num_clauses);
        for (std::uint32_t idx = 0; idx < num_clauses; ++idx) {
            all[idx] = idx;
        }
        const bool expected_sat = satisfiable(clauses, all, num_coordinates);
        const std::size_t small_cap = 1 + uniform_below(rng, 3);
        for (const std::size_t max_conflicts : {std::size_t{1000}, small_cap}) {
            // Both shrinkings start from one stream, so they solve the first time alike and
            // `none` keeps the first core that `repeat` starts from.
            const Rng start = rng;
            std::size_t first_core_size = 0;
            for (const BveCoreShrinking shrinking : {BveCoreShrinking::none, BveCoreShrinking::repeat}) {
                Rng solve_rng = start;
                const GateSearch search = search_gate(clauses, num_coordinates + 1, shrinking, max_conflicts, solve_rng);
                if (max_conflicts == 1000) {
                    REQUIRE(search.core.has_value() == !expected_sat);
                    (expected_sat ? num_sat : num_unsat) += 1;
                } else if (!expected_sat && !search.core.has_value()) {
                    ++num_capped;
                    continue;
                } else {
                    ++num_finished_under_small_cap;
                }
                REQUIRE(search.core.has_value() == !expected_sat);
                if (expected_sat) {
                    continue;
                }
                const std::vector<std::uint32_t>& core = *search.core;
                REQUIRE(std::ranges::is_sorted(core));
                if (shrinking == BveCoreShrinking::none) {
                    first_core_size = core.size();
                } else {
                    CHECK(core.size() <= first_core_size);
                    num_shrunk += core.size() < first_core_size ? 1U : 0U;
                }
                CHECK_FALSE(satisfiable(clauses, core, num_coordinates));
                // The refutation cites only the core and replays as LRUP(⊕) lines.
                for (const GateRefutation::Lemma& lemma : search.refutation.lemmas) {
                    for (const std::uint32_t reason : lemma.reasons) {
                        CHECK((reason >= num_clauses || std::ranges::binary_search(core, reason)));
                    }
                }
                CHECK(refutation_replays(clauses, search.refutation, {}, num_coordinates + 1));
                CHECK(lifted_refutation_replays(clauses, core, search.refutation, num_coordinates));
            }
        }
    }
    CHECK(num_sat > 1000);
    CHECK(num_unsat > 1000);
    CHECK(num_capped > 10);
    CHECK(num_finished_under_small_cap > 1000);
    // A repeated solve on a first core seldom shrinks it on formulas this small.
    CHECK(num_shrunk > 0);
}

TEST_CASE("gate search: the hybrid agrees with brute force, its table gates are minimal and refute") {
    Rng rng = make_rng(default_seed, RngComponent::bve_draw);
    GateTable table;
    std::size_t num_table_gates = 0;
    std::size_t num_solver_gates = 0;
    std::size_t num_sat = 0;
    std::size_t num_multi_word = 0;
    for (std::size_t round = 0; round < 2000; ++round) {
        const std::size_t num_coordinates = 1 + uniform_below(rng, 10);
        const std::size_t num_clauses = 1 + uniform_below(rng, 3 * num_coordinates + 4);
        const std::size_t max_width = 1 + uniform_below(rng, std::min<std::size_t>(num_coordinates, 6));
        std::vector<Rows> clauses;
        for (std::size_t i = 0; i < num_clauses; ++i) {
            clauses.push_back(random_clause(num_coordinates + 1, max_width, rng));
        }
        std::vector<std::uint32_t> all(num_clauses);
        std::iota(all.begin(), all.end(), std::uint32_t{0});
        const bool expected_sat = satisfiable(clauses, all, num_coordinates);
        const std::size_t num_words = std::size_t{1} << (std::max<std::size_t>(num_coordinates, 6) - 6);
        const std::size_t cost = num_clauses * num_clauses * num_words;
        for (const std::size_t max_table_cost : {std::size_t{0}, cost - 1, cost, std::numeric_limits<std::size_t>::max()}) {
            const GateSearch search = search_gate_hybrid(clauses, num_coordinates + 1, max_table_cost,
                                                         BveCoreShrinking::none, 1000, table, rng);
            CHECK(search.by_table == (max_table_cost >= cost));
            REQUIRE(search.core.has_value() == !expected_sat);
            if (expected_sat) {
                ++num_sat;
                continue;
            }
            const std::vector<std::uint32_t>& core = *search.core;
            REQUIRE(std::ranges::is_sorted(core));
            CHECK_FALSE(satisfiable(clauses, core, num_coordinates));
            if (!search.by_table) {
                ++num_solver_gates;
                continue;
            }
            ++num_table_gates;
            num_multi_word += num_words > 1 ? 1U : 0U;
            for (std::size_t dropped = 0; dropped < core.size(); ++dropped) {
                std::vector<std::uint32_t> rest = core;
                rest.erase(rest.begin() + static_cast<std::ptrdiff_t>(dropped));
                CHECK(satisfiable(clauses, rest, num_coordinates));
            }
            const GateRefutation refutation = refute_gate(clauses, num_coordinates + 1, core, rng);
            for (const GateRefutation::Lemma& lemma : refutation.lemmas) {
                for (const std::uint32_t reason : lemma.reasons) {
                    CHECK((reason >= num_clauses || std::ranges::binary_search(core, reason)));
                }
            }
            for (const std::uint32_t reason : refutation.reasons) {
                CHECK((reason >= num_clauses || std::ranges::binary_search(core, reason)));
            }
            CHECK(refutation_replays(clauses, refutation, {}, num_coordinates + 1));
            CHECK(lifted_refutation_replays(clauses, core, refutation, num_coordinates));
        }
    }
    CHECK(num_sat > 1000);
    CHECK(num_table_gates > 1000);
    CHECK(num_solver_gates > 1000);
    CHECK(num_multi_word > 100);
}

TEST_CASE("gate solver: an empty clause is a core by itself") {
    Rng rng = make_rng(default_seed, RngComponent::bve_draw);
    Row x_is_zero(3);
    x_is_zero.set_bit(0);
    const std::vector<Rows> clauses{{x_is_zero}, {}, {x_is_zero}};
    const GateSearch search = search_gate(clauses, 3, BveCoreShrinking::none, 1, rng);
    REQUIRE(search.core.has_value());
    CHECK(*search.core == std::vector<std::uint32_t>{1});
}

TEST_CASE("bve: the shared equations the flip set changes are found as brute force finds them") {
    constexpr EquationRepr repr = EquationRepr::dense;
    Rng rng = make_rng(default_seed, RngComponent::bve_draw);
    std::size_t num_shared = 0;
    std::size_t num_not_shared = 0;
    for (std::size_t round = 0; round < 2000; ++round) {
        const std::size_t num_vars = 2 + uniform_below(rng, 4);
        // The flip set changes a direction with an odd number of its variables.
        std::vector<std::uint8_t> in_flip_set(num_vars, 0);
        while (std::ranges::none_of(in_flip_set, [](std::uint8_t in) { return in != 0; })) {
            for (std::uint8_t& in : in_flip_set) {
                in = uniform_below(rng, 2) == 0 ? 1 : 0;
            }
        }
        const auto changes = [&](const Row& row) {
            bool odd = false;
            for (std::size_t var = 0; var < num_vars; ++var) {
                odd = odd != (row.get_bit(var) && in_flip_set[var] != 0);
            }
            return odd;
        };
        const auto equation_of = [&](const Row& row) {
            std::vector<std::size_t> bits;
            row.for_each_set_bit([&](std::size_t bit) { bits.push_back(bit); });
            return Equation::from_bits(repr, num_vars, bits);
        };
        std::vector<Rows> rows;
        const std::size_t num_clauses = 1 + uniform_below(rng, 3);
        while (rows.size() < num_clauses) {
            Rows clause = random_clause(num_vars + 1, 3, rng);
            if (std::ranges::any_of(clause, changes)) {
                rows.push_back(std::move(clause));
            }
        }
        std::vector<Clause> negations;
        for (const Rows& clause : rows) {
            Clause& negation = negations.emplace_back(repr, num_vars);
            for (const Row& row : clause) {
                negation.add_row(equation_of(row));
            }
        }
        // The rows of the first clause the flip set does not change span a hyperplane of it.
        Clause unchanged(repr, num_vars);
        const Rows& first = rows[0];
        const auto changed = std::ranges::find_if(first, changes);
        for (const Row& row : first) {
            Row rest = row;
            if (changes(rest)) {
                rest.plus_assign(*changed);
            }
            if (rest.leading_bit().has_value()) {
                unchanged.add_row(equation_of(rest));
            }
        }
        // Every combination of the first clause's rows, tried against the others' spans.
        bool expected = false;
        for (std::uint32_t mask = 1; mask < (std::uint32_t{1} << first.size()); ++mask) {
            Row sum(num_vars + 1);
            for (std::size_t i = 0; i < first.size(); ++i) {
                if (((mask >> i) & 1U) != 0) {
                    sum.plus_assign(first[i]);
                }
            }
            if (!changes(sum)) {
                continue;
            }
            expected = expected || std::all_of(negations.begin() + 1, negations.end(), [&](const Clause& other) {
                Equation reduced = equation_of(sum);
                return other.partial_reduce(reduced).kind == ReduceResult::Kind::eq_true;
            });
        }
        const std::vector<Equation> shared = changed_shared_equations(negations, unchanged);
        CHECK(shared.empty() == !expected);
        // Each is shared and changed, and they are independent.
        Clause shared_span(repr, num_vars);
        for (const Equation& equation : shared) {
            for (const Clause& negation : negations) {
                Equation reduced = equation;
                CHECK(negation.partial_reduce(reduced).kind == ReduceResult::Kind::eq_true);
            }
            Equation reduced = equation;
            CHECK(unchanged.partial_reduce(reduced).kind != ReduceResult::Kind::eq_true);
            shared_span.add_row(equation);
        }
        CHECK(shared_span.rows() == shared.size());
        (expected ? num_shared : num_not_shared) += 1;
    }
    CHECK(num_shared > 100);
    CHECK(num_not_shared > 100);
}

} // namespace xorcle
