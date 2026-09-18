/// @file test_ema.cpp
/// @brief The moving average, against averages worked out by hand.

#include <doctest/doctest.h>

#include "util/ema.h"

namespace xorcle {

TEST_CASE("Ema: reads back zero until it has a sample") {
    Ema ema(10);
    CHECK(ema.value() == 0.0);
    ema.update(7.0);
    CHECK(ema.value() == doctest::Approx(7.0));
}

TEST_CASE("Ema: a constant stream reads back that constant") {
    // The bias correction makes this exact, not asymptotic: an uncorrected
    // average would still be climbing towards 5 after ten samples.
    Ema fast(3);
    Ema slow(100000);
    for (int i = 0; i < 10; ++i) {
        fast.update(5.0);
        slow.update(5.0);
    }
    CHECK(fast.value() == doctest::Approx(5.0));
    CHECK(slow.value() == doctest::Approx(5.0));
}

TEST_CASE("Ema: a window of one is the last sample") {
    Ema ema(1);
    ema.update(3.0);
    ema.update(11.0);
    CHECK(ema.value() == doctest::Approx(11.0));
}

TEST_CASE("Ema: a narrow window follows a step and a wide one lags it") {
    Ema fast(10);
    Ema slow(10000);
    for (int i = 0; i < 500; ++i) {
        fast.update(1.0);
        slow.update(1.0);
    }
    for (int i = 0; i < 50; ++i) {
        fast.update(10.0);
        slow.update(10.0);
    }
    CHECK(fast.value() > 8.0);
    CHECK(slow.value() < 2.0);
}


} // namespace xorcle
