#include <doctest/doctest.h>

#include <vector>

#include "algebra_test_support.h"
#include "util/bitmap.h"

namespace xorcle {

TEST_CASE("bitmap: construction and basic access") {
    Bitmap bitmap(200);
    CHECK(bitmap.size() == 200);
    CHECK(bitmap.none());
    CHECK(bitmap.count() == 0);

    bitmap.set(0);
    bitmap.set(63);
    bitmap.set(64);
    bitmap.set(199);
    CHECK_FALSE(bitmap.none());
    CHECK(bitmap.count() == 4);
    CHECK(bitmap.test(0));
    CHECK(bitmap.test(63));
    CHECK(bitmap.test(64));
    CHECK(bitmap.test(199));
    CHECK_FALSE(bitmap.test(1));

    bitmap.reset(64);
    CHECK_FALSE(bitmap.test(64));
    CHECK(bitmap.count() == 3);

    bitmap.clear();
    CHECK(bitmap.none());
    CHECK(bitmap.size() == 200);

    bitmap.reset_width(10);
    CHECK(bitmap.size() == 10);
    CHECK(bitmap.none());
}

TEST_CASE("bitmap: count_below gives row-echelon insertion indices") {
    Bitmap bitmap(300);
    const std::vector<std::size_t> set_columns{2, 5, 63, 64, 128, 200, 299};
    for (const std::size_t column : set_columns) {
        bitmap.set(column);
    }

    // count_below(i) must equal the number of set columns strictly below i,
    // which is exactly where a row leading on column i is inserted.
    for (std::size_t i = 0; i <= bitmap.size(); ++i) {
        std::size_t expected = 0;
        for (const std::size_t column : set_columns) {
            if (column < i) {
                ++expected;
            }
        }
        CHECK(bitmap.count_below(i) == expected);
    }
    CHECK(bitmap.count_below(bitmap.size()) == bitmap.count());
}

TEST_CASE("bitmap: for_each_set visits set columns in ascending order") {
    Bitmap bitmap(400);
    const std::vector<std::size_t> expected{0, 1, 64, 65, 191, 256, 399};
    for (const std::size_t column : expected) {
        bitmap.set(column);
    }

    std::vector<std::size_t> seen;
    bitmap.for_each_set([&](std::size_t column) { seen.push_back(column); });
    CHECK(seen == expected);
}

TEST_CASE("bitmap: equality and randomized agreement with a reference") {
    TestRandom random(555);
    constexpr std::size_t width = 260;

    for (int trial = 0; trial < 100; ++trial) {
        Bitmap bitmap(width);
        std::vector<bool> reference(width, false);
        for (std::size_t column = 0; column < width; ++column) {
            if (random.chance(3)) {
                bitmap.set(column);
                reference[column] = true;
            }
        }

        std::size_t expected_count = 0;
        for (std::size_t column = 0; column < width; ++column) {
            REQUIRE(bitmap.test(column) == reference[column]);
            if (reference[column]) {
                ++expected_count;
            }
            CHECK(bitmap.count_below(column) ==
                  static_cast<std::size_t>(expected_count - (reference[column] ? 1 : 0)));
        }
        CHECK(bitmap.count() == expected_count);
        CHECK(bitmap.none() == (expected_count == 0));

        const Bitmap copy = bitmap;
        CHECK(copy == bitmap);
    }
}

} // namespace xorcle
