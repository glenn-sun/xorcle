/// @file test_random.cpp
/// @brief Pins the RNG stream derivation to fixed, externally measured values.
///
/// Every expected value here was produced by running a separate reference
/// implementation of `derive_seed`, `random_int` and
/// `random_nonzero_coefficients`, not by running the code under test, so these
/// are real fixtures rather than a snapshot of current behaviour. The
/// differential harness requires identical conflict counts from matching seeds,
/// which requires identical streams.

#include <doctest/doctest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "util/random.h"

namespace xorcle {

namespace {

/// @brief Render a coefficient column set as '0'/'1' in ascending index order.
std::string to_bit_string(const IndexSet& col_idxs) {
    std::string bits;
    bits.reserve(col_idxs.size());
    for (std::size_t i = 0; i < col_idxs.size(); ++i) {
        bits.push_back(col_idxs.test(i) ? '1' : '0');
    }
    return bits;
}

} // namespace

TEST_CASE("random: the component tags keep their fixed values") {
    // A component tag identifies a stream, so renaming the enumerator must not
    // change its value.
    CHECK(static_cast<std::uint64_t>(RngComponent::decision_phase) == 0x6a09e667f3bcc909ULL);
    CHECK(static_cast<std::uint64_t>(RngComponent::csids_equation) == 0xbb67ae8584caa73bULL);
    CHECK(static_cast<std::uint64_t>(RngComponent::minimize_representative) ==
          0x3c6ef372fe94f82bULL);
    CHECK(static_cast<std::uint64_t>(RngComponent::implication_graph_sample) ==
          0xa54ff53a5f1d36f1ULL);
    CHECK(default_seed == 0u);
}

TEST_CASE("random: derive_seed reproduces its reference values") {
    struct Case {
        Seed base;
        RngComponent component;
        Seed expected;
    };

    const std::vector<Case> cases = {
        {0, RngComponent::decision_phase, 10870759137850584762ULL},
        {0, RngComponent::csids_equation, 17133378899364576809ULL},
        {1, RngComponent::decision_phase, 4995873258355806544ULL},
        {1, RngComponent::csids_equation, 16237881576802071655ULL},
        {42, RngComponent::decision_phase, 12247817420404419398ULL},
        {42, RngComponent::csids_equation, 186222749232901458ULL},
        {12345, RngComponent::decision_phase, 3235331413191061458ULL},
        {12345, RngComponent::csids_equation, 6898429099592197767ULL},
        {18446744073709551615ULL, RngComponent::decision_phase, 3189705271989274347ULL},
        {18446744073709551615ULL, RngComponent::csids_equation, 12351632284671268914ULL},
    };

    for (const Case& c : cases) {
        CAPTURE(c.base);
        CHECK(derive_seed(c.base, c.component) == c.expected);
    }

    // The two component streams are distinct for every seed tested.
    for (Seed base : {Seed{0}, Seed{1}, Seed{42}, Seed{12345}}) {
        CHECK(derive_seed(base, RngComponent::decision_phase) !=
              derive_seed(base, RngComponent::csids_equation));
    }
}

TEST_CASE("random: make_rng seeds from derive_seed") {
    Rng expected(derive_seed(42, RngComponent::csids_equation));
    Rng actual = make_rng(42, RngComponent::csids_equation);
    for (int i = 0; i < 8; ++i) {
        CHECK(actual() == expected());
    }
}

TEST_CASE("random: random_int reproduces its reference phase draws") {
    struct Case {
        Seed base;
        const char* draws;
    };

    // 32 consecutive `random_int(rng, 2)` draws off the decision_phase stream.
    const std::vector<Case> cases = {
        {0, "01011000110101001100101001000110"},
        {1, "00001010010011010011000111111000"},
        {42, "11101101100111001100010100010010"},
    };

    for (const Case& c : cases) {
        CAPTURE(c.base);
        Rng rng = make_rng(c.base, RngComponent::decision_phase);
        for (const char* p = c.draws; *p != '\0'; ++p) {
            CHECK(random_int(rng, 2) == (*p - '0'));
        }
    }
}

TEST_CASE("random: random_nonzero_coefficients reproduces its reference draws") {
    // First eight samples off a fresh csids_equation stream, as bit strings in
    // ascending index order.
    SUBCASE("seed 0, num_rows 1") {
        Rng rng = make_rng(0, RngComponent::csids_equation);
        // Rejection sampling on a single bit: every accepted sample is "1".
        for (int i = 0; i < 8; ++i) {
            CHECK(to_bit_string(random_nonzero_coefficients(1, rng)) == "1");
        }
    }

    SUBCASE("seed 0, num_rows 3") {
        const std::vector<std::string> expected = {"101", "010", "011", "010",
                                                   "101", "110", "010", "101"};
        Rng rng = make_rng(0, RngComponent::csids_equation);
        for (const std::string& want : expected) {
            CHECK(to_bit_string(random_nonzero_coefficients(3, rng)) == want);
        }
    }

    SUBCASE("seed 42, num_rows 2") {
        const std::vector<std::string> expected = {"11", "10", "10", "10",
                                                   "10", "01", "11", "11"};
        Rng rng = make_rng(42, RngComponent::csids_equation);
        for (const std::string& want : expected) {
            CHECK(to_bit_string(random_nonzero_coefficients(2, rng)) == want);
        }
    }

    SUBCASE("seed 0, num_rows 64 -- exactly one draw per attempt") {
        const std::vector<std::string> expected = {
            "0001100100010100010101101110100100101111011100100101011000010000",
            "1010100100101001100010110010100001011001110001100011111010000011",
            "0101001100011010001011101100001101001011111110010000011110000001",
            "0110000110001000000000111011100110111101101100100110001010110010",
        };
        Rng rng = make_rng(0, RngComponent::csids_equation);
        for (const std::string& want : expected) {
            CHECK(to_bit_string(random_nonzero_coefficients(64, rng)) == want);
        }
    }

    SUBCASE("seed 42, num_rows 63 -- one draw per attempt, top index dropped") {
        const std::vector<std::string> expected = {
            "111110011011010010111111000001110001111100011011111111101001111",
            "100010110010111101000111001100111110100001000011100110100011100",
            "100110111110010101000010110000000111010111011100010100011001111",
        };
        Rng rng = make_rng(42, RngComponent::csids_equation);
        for (const std::string& want : expected) {
            CHECK(to_bit_string(random_nonzero_coefficients(63, rng)) == want);
        }
    }

    SUBCASE("seed 0, num_rows 65 -- two draws per attempt, tail indices dropped") {
        // Each sample's first 64 indices are one whole draw, so sample k here uses
        // draws 2k-1 and 2k of the same stream the num_rows-64 case walks one draw
        // at a time.
        const std::vector<std::string> expected = {
            "0001100100010100010101101110100100101111011100100101011000010000"
            "1",
            "0101001100011010001011101100001101001011111110010000011110000001"
            "0",
            "0101101000001101100011110101101001000010010000010011110101010001"
            "1",
            "1100111001001000100000001010100001001101101101010011110101100010"
            "0",
        };
        Rng rng = make_rng(0, RngComponent::csids_equation);
        for (const std::string& want : expected) {
            CHECK(to_bit_string(random_nonzero_coefficients(65, rng)) == want);
        }
    }

    SUBCASE("seed 0, num_rows 100 -- the high 28 indices of the second draw are dropped") {
        const std::vector<std::string> expected = {
            "0001100100010100010101101110100100101111011100100101011000010000"
            "101010010010100110001011001010000101",
            "0101001100011010001011101100001101001011111110010000011110000001"
            "011000011000100000000011101110011011",
        };
        Rng rng = make_rng(0, RngComponent::csids_equation);
        for (const std::string& want : expected) {
            CHECK(to_bit_string(random_nonzero_coefficients(100, rng)) == want);
        }
    }

    SUBCASE("seed 0, num_rows 129 -- three draws per attempt") {
        const std::vector<std::string> expected = {
            "0001100100010100010101101110100100101111011100100101011000010000"
            "1010100100101001100010110010100001011001110001100011111010000011"
            "0",
            "0110000110001000000000111011100110111101101100100110001010110010"
            "0101101000001101100011110101101001000010010000010011110101010001"
            "1",
        };
        Rng rng = make_rng(0, RngComponent::csids_equation);
        for (const std::string& want : expected) {
            CHECK(to_bit_string(random_nonzero_coefficients(129, rng)) == want);
        }
    }
}

TEST_CASE("random: the number of draws consumed matches the reference stream") {
    // The state of the shared stream after eight samples is what the
    // differential harness depends on.
    struct Case {
        std::size_t num_rows;
        std::uint64_t next_draw;
    };

    const std::vector<Case> cases = {
        {1, 17698327589968659146ULL},
        {2, 119965670991748851ULL},
        {3, 119965670991748851ULL},
        {63, 4502100644813836970ULL},
        {64, 4502100644813836970ULL},
        {65, 11380781191505358918ULL},
        {100, 11380781191505358918ULL},
        {128, 11380781191505358918ULL},
        {129, 3683860953314191206ULL},
        {256, 4259575628509358580ULL},
    };

    for (const Case& c : cases) {
        CAPTURE(c.num_rows);
        Rng rng = make_rng(0, RngComponent::csids_equation);
        for (int i = 0; i < 8; ++i) {
            (void)random_nonzero_coefficients(c.num_rows, rng);
        }
        CHECK(rng() == c.next_draw);
    }
}

TEST_CASE("random: random_nonzero_coefficients postconditions") {
    Rng rng = make_rng(7, RngComponent::csids_equation);
    for (std::size_t num_rows : {std::size_t{1}, std::size_t{2}, std::size_t{17}, std::size_t{64},
                                 std::size_t{65}, std::size_t{200}}) {
        CAPTURE(num_rows);
        for (int trial = 0; trial < 32; ++trial) {
            const IndexSet coefficients = random_nonzero_coefficients(num_rows, rng);
            CHECK(coefficients.size() == num_rows);
            CHECK_FALSE(coefficients.none());
            CHECK(coefficients.count() <= num_rows);
        }
    }
}

} // namespace xorcle
