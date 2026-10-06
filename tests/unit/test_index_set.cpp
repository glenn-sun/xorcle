#include <doctest/doctest.h>

#include <vector>

#include "algebra_test_support.h"
#include "allocation_counter.h"
#include "util/index_set.h"

namespace xorcle {

TEST_CASE("index_set: construction and basic access") {
    IndexSet col_idxs(200);
    CHECK(col_idxs.size() == 200);
    CHECK(col_idxs.none());
    CHECK(col_idxs.count() == 0);

    col_idxs.set(0);
    col_idxs.set(63);
    col_idxs.set(64);
    col_idxs.set(199);
    CHECK_FALSE(col_idxs.none());
    CHECK(col_idxs.count() == 4);
    CHECK(col_idxs.test(0));
    CHECK(col_idxs.test(63));
    CHECK(col_idxs.test(64));
    CHECK(col_idxs.test(199));
    CHECK_FALSE(col_idxs.test(1));

    col_idxs.reset(64);
    CHECK_FALSE(col_idxs.test(64));
    CHECK(col_idxs.count() == 3);

    col_idxs.clear();
    CHECK(col_idxs.none());
    CHECK(col_idxs.size() == 200);
}

TEST_CASE("index_set: resize empties and or_with unions") {
    IndexSet row_idxs(200);
    row_idxs.set(3);
    row_idxs.set(199);

    row_idxs.resize(70);
    CHECK(row_idxs.size() == 70);
    CHECK(row_idxs.none());

    row_idxs.resize(300);
    CHECK(row_idxs.size() == 300);
    CHECK(row_idxs.none());

    // Shrinking to a size already held leaves the storage alone.
    const std::size_t before = allocation_count();
    row_idxs.resize(70);
    CHECK(allocation_count() == before);

    row_idxs.set(1);
    row_idxs.set(69);
    IndexSet other(70);
    other.set(1);
    other.set(64);
    row_idxs.or_with(other);
    CHECK(row_idxs.count() == 3);
    CHECK(row_idxs.test(1));
    CHECK(row_idxs.test(64));
    CHECK(row_idxs.test(69));
}

TEST_CASE("index_set: intersects finds a shared index in any word") {
    IndexSet col_idxs(130);
    col_idxs.set(3);
    col_idxs.set(129);
    IndexSet other(130);
    CHECK_FALSE(col_idxs.intersects(other));
    other.set(4);
    other.set(128);
    CHECK_FALSE(col_idxs.intersects(other));
    other.set(129);
    CHECK(col_idxs.intersects(other));
    CHECK(other.intersects(col_idxs));
}

TEST_CASE("index_set: count_below gives row-echelon insertion indices") {
    IndexSet col_idxs(300);
    const std::vector<std::size_t> set_columns{2, 5, 63, 64, 128, 200, 299};
    for (const std::size_t col_idx : set_columns) {
        col_idxs.set(col_idx);
    }

    // count_below(i) must equal the number of set columns strictly below i,
    // which is exactly where a row leading on column i is inserted.
    for (std::size_t i = 0; i <= col_idxs.size(); ++i) {
        std::size_t expected = 0;
        for (const std::size_t col_idx : set_columns) {
            if (col_idx < i) {
                ++expected;
            }
        }
        CHECK(col_idxs.count_below(i) == expected);
    }
    CHECK(col_idxs.count_below(col_idxs.size()) == col_idxs.count());
}

TEST_CASE("index_set: for_each_set visits set columns in ascending order") {
    IndexSet col_idxs(400);
    const std::vector<std::size_t> expected{0, 1, 64, 65, 191, 256, 399};
    for (const std::size_t col_idx : expected) {
        col_idxs.set(col_idx);
    }

    std::vector<std::size_t> seen;
    col_idxs.for_each_set([&](std::size_t col_idx) { seen.push_back(col_idx); });
    CHECK(seen == expected);
}

TEST_CASE("index_set: equality and randomized agreement with a reference") {
    TestRandom random(555);
    constexpr std::size_t num_columns = 260;

    for (int trial = 0; trial < 100; ++trial) {
        IndexSet col_idxs(num_columns);
        std::vector<bool> reference(num_columns, false);
        for (std::size_t col_idx = 0; col_idx < num_columns; ++col_idx) {
            if (random.chance(3)) {
                col_idxs.set(col_idx);
                reference[col_idx] = true;
            }
        }

        std::size_t expected_count = 0;
        for (std::size_t col_idx = 0; col_idx < num_columns; ++col_idx) {
            REQUIRE(col_idxs.test(col_idx) == reference[col_idx]);
            if (reference[col_idx]) {
                ++expected_count;
            }
            CHECK(col_idxs.count_below(col_idx) ==
                  static_cast<std::size_t>(expected_count - (reference[col_idx] ? 1 : 0)));
        }
        CHECK(col_idxs.count() == expected_count);
        CHECK(col_idxs.none() == (expected_count == 0));

        const IndexSet copy = col_idxs;
        CHECK(copy == col_idxs);
    }
}

} // namespace xorcle
