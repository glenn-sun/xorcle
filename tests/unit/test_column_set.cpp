#include <doctest/doctest.h>

#include <vector>

#include "algebra_test_support.h"
#include "util/column_set.h"

namespace xorcle {

TEST_CASE("column_set: construction and basic access") {
    ColumnSet columns(200);
    CHECK(columns.size() == 200);
    CHECK(columns.none());
    CHECK(columns.count() == 0);

    columns.set(0);
    columns.set(63);
    columns.set(64);
    columns.set(199);
    CHECK_FALSE(columns.none());
    CHECK(columns.count() == 4);
    CHECK(columns.test(0));
    CHECK(columns.test(63));
    CHECK(columns.test(64));
    CHECK(columns.test(199));
    CHECK_FALSE(columns.test(1));

    columns.reset(64);
    CHECK_FALSE(columns.test(64));
    CHECK(columns.count() == 3);

    columns.clear();
    CHECK(columns.none());
    CHECK(columns.size() == 200);

    columns.reset_width(10);
    CHECK(columns.size() == 10);
    CHECK(columns.none());
}

TEST_CASE("column_set: count_below gives row-echelon insertion indices") {
    ColumnSet columns(300);
    const std::vector<std::size_t> set_columns{2, 5, 63, 64, 128, 200, 299};
    for (const std::size_t column : set_columns) {
        columns.set(column);
    }

    // count_below(i) must equal the number of set columns strictly below i,
    // which is exactly where a row leading on column i is inserted.
    for (std::size_t i = 0; i <= columns.size(); ++i) {
        std::size_t expected = 0;
        for (const std::size_t column : set_columns) {
            if (column < i) {
                ++expected;
            }
        }
        CHECK(columns.count_below(i) == expected);
    }
    CHECK(columns.count_below(columns.size()) == columns.count());
}

TEST_CASE("column_set: for_each_set visits set columns in ascending order") {
    ColumnSet columns(400);
    const std::vector<std::size_t> expected{0, 1, 64, 65, 191, 256, 399};
    for (const std::size_t column : expected) {
        columns.set(column);
    }

    std::vector<std::size_t> seen;
    columns.for_each_set([&](std::size_t column) { seen.push_back(column); });
    CHECK(seen == expected);
}

TEST_CASE("column_set: equality and randomized agreement with a reference") {
    TestRandom random(555);
    constexpr std::size_t width = 260;

    for (int trial = 0; trial < 100; ++trial) {
        ColumnSet columns(width);
        std::vector<bool> reference(width, false);
        for (std::size_t column = 0; column < width; ++column) {
            if (random.chance(3)) {
                columns.set(column);
                reference[column] = true;
            }
        }

        std::size_t expected_count = 0;
        for (std::size_t column = 0; column < width; ++column) {
            REQUIRE(columns.test(column) == reference[column]);
            if (reference[column]) {
                ++expected_count;
            }
            CHECK(columns.count_below(column) ==
                  static_cast<std::size_t>(expected_count - (reference[column] ? 1 : 0)));
        }
        CHECK(columns.count() == expected_count);
        CHECK(columns.none() == (expected_count == 0));

        const ColumnSet copy = columns;
        CHECK(copy == columns);
    }
}

} // namespace xorcle
