#include <doctest/doctest.h>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <set>
#include <vector>

#include "algebra/equation.h"
#include "algebra/equation_repr.h"
#include "algebra/row_echelon_matrix.h"
#include "algebra_test_support.h"
#include "core/inproc_subsume_strengthen.h"
#include "heuristics_test_support.h"
#include "util/random.h"

namespace xorcle {

namespace {

/// @brief An equation's bits as a mask, constant bit included.
std::uint64_t mask_of(const Equation& equation) {
    std::uint64_t mask = 0;
    for (const std::size_t bit : bits_of(equation)) {
        mask |= std::uint64_t{1} << bit;
    }
    return mask;
}

/// @brief The assignments over @p num_vars variables, each a mask, on which every
/// row of @p clause holds: the ones the clause forbids.
std::set<std::uint64_t> forbidden(const Clause& clause, const std::size_t num_vars) {
    std::set<std::uint64_t> assignments;
    const std::uint64_t constant = std::uint64_t{1} << num_vars;
    for (std::uint64_t x = 0; x < constant; ++x) {
        bool all_hold = true;
        for (const Equation& row : clause) {
            all_hold = all_hold && (std::popcount(mask_of(row) & (x | constant)) & 1) == 0;
        }
        if (all_hold) {
            assignments.insert(x);
        }
    }
    return assignments;
}

/// @brief Whether C and D together imply H, read off the truth tables.
bool implied(const std::set<std::uint64_t>& forbidden_c, const std::set<std::uint64_t>& forbidden_d,
             const std::set<std::uint64_t>& forbidden_h) {
    for (const std::uint64_t x : forbidden_h) {
        if (!forbidden_c.contains(x) && !forbidden_d.contains(x)) {
            return false;
        }
    }
    return true;
}

/// @brief A clause of up to @p max_width random rows over @p num_vars variables,
/// in reduced row-echelon form; empty if its span holds `0 = 1`.
Clause random_clause(const EquationRepr repr, const std::size_t num_vars,
                     const std::size_t max_width, Rng& rng) {
    Clause clause(repr, num_vars);
    const std::size_t width = 1 + uniform_below(rng, max_width);
    for (std::size_t i = 0; i < width; ++i) {
        std::vector<std::size_t> bits;
        for (std::size_t bit = 0; bit <= num_vars; ++bit) {
            if (uniform_below(rng, 2) == 1) {
                bits.push_back(bit);
            }
        }
        Equation row(repr, num_vars);
        row.assign_bits(bits);
        if (!row.is_eq_true()) {
            clause.add_row(std::move(row));
        }
    }
    clause.reduce_rows();
    if (clause.rows() == 0 || clause.pivot_col_idxs().test(num_vars)) {
        return Clause(repr, num_vars);
    }
    return clause;
}

/// @brief Every hyperplane of the span of @p clause, as the forbidden sets of its clauses.
std::vector<std::set<std::uint64_t>> hyperplanes(const Clause& clause, const std::size_t num_vars) {
    std::vector<std::set<std::uint64_t>> result;
    const std::size_t width = clause.rows();
    for (std::uint64_t functional = 1; functional < (std::uint64_t{1} << width); ++functional) {
        // The kernel of the functional: every combination of the rows it sends to 0.
        Clause kernel(clause.repr(), num_vars);
        for (std::uint64_t combination = 1; combination < (std::uint64_t{1} << width);
             ++combination) {
            if ((std::popcount(combination & functional) & 1) != 0) {
                continue;
            }
            Equation row(clause.repr(), num_vars);
            for (std::size_t i = 0; i < width; ++i) {
                if ((combination >> i & 1) != 0) {
                    row.plus_assign(clause.row(i));
                }
            }
            kernel.add_row(std::move(row));
        }
        result.push_back(forbidden(kernel, num_vars));
    }
    return result;
}

} // namespace

// C subsumes D when C implies D, and strengthens D when C and D together imply a
// hyperplane of D's span; checked against truth tables over a few variables.
TEST_CASE_TEMPLATE("inprocessing subsumption: a pair's relation and its hyperplanes match truth tables",
                   Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    Rng rng(7);
    std::size_t num_subsumed = 0;
    std::size_t num_strengthened = 0;
    std::size_t num_neither = 0;
    for (std::size_t trial = 0; trial < 3000; ++trial) {
        const std::size_t num_vars = 3 + uniform_below(rng, 3);
        const Clause narrower = random_clause(repr, num_vars, 3, rng);
        const Clause wider = random_clause(repr, num_vars, 4, rng);
        if (narrower.rows() == 0 || wider.rows() == 0 || narrower.rows() > wider.rows()) {
            continue;
        }
        CAPTURE(trial);
        const std::set<std::uint64_t> forbidden_c = forbidden(narrower, num_vars);
        const std::set<std::uint64_t> forbidden_d = forbidden(wider, num_vars);

        PairCheck check(repr, num_vars);
        const PairRelation relation = check.relate(narrower, wider);
        // C implies D exactly when D forbids nothing C allows.
        const bool subsumes = implied(forbidden_c, {}, forbidden_d);
        std::vector<std::set<std::uint64_t>> valid;
        for (const std::set<std::uint64_t>& hyperplane : hyperplanes(wider, num_vars)) {
            if (implied(forbidden_c, forbidden_d, hyperplane)) {
                valid.push_back(hyperplane);
            }
        }
        if (subsumes) {
            CHECK(relation == PairRelation::subsumes);
            ++num_subsumed;
            continue;
        }
        if (valid.empty()) {
            CHECK(relation == PairRelation::none);
            ++num_neither;
            continue;
        }
        REQUIRE(relation == PairRelation::strengthens);
        ++num_strengthened;
        CHECK(valid.size() == std::size_t{1} << (wider.rows() - narrower.rows()));
        // Every draw is a valid hyperplane, and over enough draws each one comes up.
        std::set<std::set<std::uint64_t>> drawn;
        for (std::size_t draw = 0; draw < 64; ++draw) {
            const Clause hyperplane = check.strengthened(wider, rng);
            CHECK(hyperplane.rows() + 1 == wider.rows());
            const std::set<std::uint64_t> forbidden_h = forbidden(hyperplane, num_vars);
            CHECK(implied(forbidden_c, forbidden_d, forbidden_h));
            drawn.insert(forbidden_h);
        }
        if (valid.size() <= 4) {
            CHECK(drawn.size() == valid.size());
        }
    }
    CHECK(num_subsumed > 50);
    CHECK(num_strengthened > 50);
    CHECK(num_neither > 50);
}

TEST_CASE("inprocessing subsumption: the rows a check reads are counted") {
    // C's one row x0 + x1 against D's rows x0 + 1 and x1: both read, leaving `0 = 1`.
    const Clause narrower = make_clause(EquationRepr::dense, 2, {{0, 1}});
    Clause wider = make_clause(EquationRepr::dense, 2, {{0, 2}, {1}});
    wider.reduce_rows();
    PairCheck check(EquationRepr::dense, 2);
    CHECK(check.relate(narrower, wider) == PairRelation::strengthens);
    CHECK(check.num_rows_read() == 2);
}

} // namespace xorcle
