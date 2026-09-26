/// @file test_max_heap.cpp
/// @brief The activity-keyed max-heap.

#include <doctest/doctest.h>

#include <optional>
#include <utility>
#include <vector>

#include "util/max_heap.h"

namespace xorcle {

namespace {

using Key = MaxHeap::Key;
using Order = std::vector<Key>;

/// @brief Drain a heap into the order it hands keys back.
Order drain(MaxHeap& heap) {
    Order order;
    while (std::optional<Key> next = heap.extract_max()) {
        order.push_back(*next);
    }
    return order;
}

} // namespace

TEST_CASE("MaxHeap: an empty heap") {
    MaxHeap heap;
    CHECK(heap.empty());
    CHECK_FALSE(heap.extract_max().has_value());
    CHECK(heap.activity(3) == 0.0);
}

TEST_CASE("MaxHeap: keys come out in descending activity") {
    const std::vector<std::pair<Key, double>> entries = {
        {10, 1.0}, {20, 5.0}, {30, 3.0}, {40, 4.0}, {50, 2.0}};

    MaxHeap heap;
    for (const auto& [key, activity] : entries) {
        heap.set_activity(key, activity);
        heap.insert(key);
    }
    CHECK(heap.peek_max() == 20);
    CHECK(drain(heap) == Order{20, 40, 30, 50, 10});
    CHECK(heap.empty());
}

TEST_CASE("MaxHeap: set_activity without insert registers a score only") {
    MaxHeap heap;
    heap.set_activity(7, 9.0);
    CHECK(heap.activity(7) == 9.0);
    CHECK(heap.empty());

    heap.insert(7);
    // A second insert of a live key is a no-op rather than a duplicate.
    heap.insert(7);
    CHECK(drain(heap) == Order{7});
}

TEST_CASE("MaxHeap: increase_key reorders live keys") {
    MaxHeap heap;
    for (Key key : {Key{1}, Key{2}, Key{3}}) {
        heap.set_activity(key, static_cast<double>(key));
        heap.insert(key);
    }
    CHECK(heap.peek_max() == 3);

    heap.increase_key(1, 10.0);
    CHECK(heap.activity(1) == 11.0);
    CHECK(heap.peek_max() == 1);

    CHECK(drain(heap) == Order{1, 3, 2});
}

TEST_CASE("MaxHeap: increase_key on an extracted key still records the score") {
    MaxHeap heap;
    heap.set_activity(1, 1.0);
    heap.set_activity(2, 2.0);
    heap.insert(1);
    heap.insert(2);

    CHECK(heap.extract_max() == 2);
    heap.increase_key(2, 100.0);
    CHECK(heap.activity(2) == 102.0);

    heap.insert(2);
    CHECK(heap.peek_max() == 2);
}

TEST_CASE("MaxHeap: an extracted key stays out and keeps its activity") {
    MaxHeap heap;
    for (Key key : {Key{1}, Key{2}, Key{3}}) {
        heap.set_activity(key, static_cast<double>(key));
        heap.insert(key);
    }
    CHECK(heap.extract_max() == 3);
    CHECK(heap.extract_max() == 2);

    heap.insert(3);
    CHECK(heap.activity(2) == 2.0);
    CHECK(drain(heap) == Order{3, 1});
}

TEST_CASE("MaxHeap: rescale preserves the order") {
    MaxHeap heap;
    for (Key key : {Key{1}, Key{2}, Key{3}}) {
        heap.set_activity(key, static_cast<double>(key) * 1e100);
        heap.insert(key);
    }
    heap.rescale(1e-100);
    CHECK(heap.activity(1) == doctest::Approx(1.0));
    CHECK(heap.activity(3) == doctest::Approx(3.0));
    CHECK(drain(heap) == Order{3, 2, 1});
}

TEST_CASE("MaxHeap: reset clears activities as well as the heap") {
    MaxHeap heap;
    heap.set_activity(1, 1.0);
    heap.insert(1);
    CHECK(heap.extract_max() == 1);

    heap.reset();
    CHECK(heap.empty());
    CHECK(heap.activity(1) == 0.0);
}

TEST_CASE("MaxHeap: equal activities come back by ascending key") {
    const std::vector<Order> insertion_orders = {
        {1, 2, 3, 4, 5, 6}, {6, 5, 4, 3, 2, 1}, {4, 1, 6, 3, 5, 2}};
    for (const Order& insertions : insertion_orders) {
        MaxHeap heap;
        for (Key key : insertions) {
            heap.set_activity(key, key % 2 == 0 ? 2.0 : 1.0);
            heap.insert(key);
        }
        CHECK(heap.peek_max() == 2);
        CHECK(drain(heap) == Order{2, 4, 6, 1, 3, 5});
    }
}

TEST_CASE("MaxHeap: a rescale that brings activities together reranks them") {
    MaxHeap heap;
    for (Key key : {Key{1}, Key{2}, Key{3}, Key{4}}) {
        heap.set_activity(key, static_cast<double>(key));
        heap.insert(key);
    }
    CHECK(heap.peek_max() == 4);

    heap.rescale(0.0);
    CHECK(drain(heap) == Order{1, 2, 3, 4});
}

} // namespace xorcle
