/// @file test_max_heap.cpp
/// @brief The activity-keyed max-heap.

#include <doctest/doctest.h>

#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "util/max_heap.h"

namespace xorcle {

namespace {

/// @brief Drain a heap into the order it hands elements back.
template <typename Heap>
std::vector<int> drain(Heap& heap) {
    std::vector<int> order;
    while (std::optional<int> next = heap.extract_max()) {
        order.push_back(*next);
    }
    return order;
}

} // namespace

TEST_CASE("MaxHeap: an empty heap") {
    MaxHeap<int> heap;
    CHECK(heap.empty());
    CHECK(heap.size() == 0);
    CHECK_FALSE(heap.extract_max().has_value());
    CHECK(heap.get_priority(3) == 0.0);
}

TEST_CASE("MaxHeap: elements come out in descending activity") {
    const std::vector<std::pair<int, double>> entries = {
        {10, 1.0}, {20, 5.0}, {30, 3.0}, {40, 4.0}, {50, 2.0}};

    MaxHeap<int> heap;
    for (const auto& [id, activity] : entries) {
        heap.set_activity(id, activity);
        heap.insert(id);
    }
    CHECK(heap.size() == 5);
    CHECK(heap.peek_max() == 20);
    CHECK(drain(heap) == std::vector<int>{20, 40, 30, 50, 10});
    CHECK(heap.empty());
}

TEST_CASE("MaxHeap: set_activity without insert registers a score only") {
    MaxHeap<int> heap;
    heap.set_activity(7, 9.0);
    CHECK(heap.get_priority(7) == 9.0);
    CHECK(heap.empty());

    heap.insert(7);
    CHECK(heap.size() == 1);
    // A second insert of a live element is a no-op.
    heap.insert(7);
    CHECK(heap.size() == 1);
}

TEST_CASE("MaxHeap: increase_key reorders live elements") {
    MaxHeap<int> heap;
    for (int id : {1, 2, 3}) {
        heap.set_activity(id, static_cast<double>(id));
        heap.insert(id);
    }
    CHECK(heap.peek_max() == 3);

    heap.increase_key(1, 10.0);
    CHECK(heap.get_priority(1) == 11.0);
    CHECK(heap.peek_max() == 1);

    // A negative delta is ignored outright.
    heap.increase_key(1, -100.0);
    CHECK(heap.get_priority(1) == 11.0);

    CHECK(drain(heap) == std::vector<int>{1, 3, 2});
}

TEST_CASE("MaxHeap: increase_key on an extracted element still records the score") {
    MaxHeap<int> heap;
    heap.set_activity(1, 1.0);
    heap.set_activity(2, 2.0);
    heap.insert(1);
    heap.insert(2);

    CHECK(heap.extract_max() == 2);
    heap.increase_key(2, 100.0);
    CHECK(heap.get_priority(2) == 102.0);
    CHECK(heap.size() == 1);

    // rebuild() brings it back, now at the top.
    heap.rebuild();
    CHECK(heap.size() == 2);
    CHECK(heap.peek_max() == 2);
}

TEST_CASE("MaxHeap: rebuild restores every popped element exactly once") {
    MaxHeap<int> heap;
    for (int id = 0; id < 8; ++id) {
        heap.set_activity(id, static_cast<double>(id));
        heap.insert(id);
    }
    for (int i = 0; i < 5; ++i) {
        CHECK(heap.extract_max().has_value());
    }
    CHECK(heap.size() == 3);

    heap.rebuild();
    CHECK(heap.size() == 8);
    // A second rebuild has nothing left to do.
    heap.rebuild();
    CHECK(heap.size() == 8);
    CHECK(drain(heap) == std::vector<int>{7, 6, 5, 4, 3, 2, 1, 0});
}

TEST_CASE("MaxHeap: rescale preserves the order") {
    MaxHeap<int> heap;
    for (int id : {1, 2, 3}) {
        heap.set_activity(id, static_cast<double>(id) * 1e100);
        heap.insert(id);
    }
    heap.rescale(1e-100);
    CHECK(heap.get_priority(1) == doctest::Approx(1.0));
    CHECK(heap.get_priority(3) == doctest::Approx(3.0));
    CHECK(drain(heap) == std::vector<int>{3, 2, 1});
}

TEST_CASE("MaxHeap: reset clears activities as well as the heap") {
    MaxHeap<int> heap;
    heap.set_activity(1, 1.0);
    heap.insert(1);
    CHECK(heap.extract_max() == 1);

    heap.reset();
    CHECK(heap.empty());
    CHECK(heap.get_priority(1) == 0.0);
    // The popped list went with it, so rebuild() finds nothing.
    heap.rebuild();
    CHECK(heap.empty());
}

TEST_CASE("MaxHeap: snapshot round-trips") {
    MaxHeap<int> heap;
    for (int id : {1, 2, 3, 4}) {
        heap.set_activity(id, static_cast<double>(id));
        heap.insert(id);
    }
    CHECK(heap.extract_max() == 4);

    auto state = heap.snapshot();
    CHECK(state.heap.size() == 3);
    CHECK(state.popped == std::vector<int>{4});
    CHECK(state.activity.size() == 4);

    MaxHeap<int> restored;
    restored.load_snapshot(state);
    CHECK(restored.size() == 3);
    CHECK(restored.peek_max() == 3);
    restored.rebuild();
    CHECK(restored.size() == 4);
    CHECK(drain(restored) == std::vector<int>{4, 3, 2, 1});
}

TEST_CASE("MaxHeap: load_snapshot drops keys a remap collapsed or orphaned") {
    // Compaction rewrites every key, which can map two distinct keys onto one
    // and can drop a key entirely. `load_snapshot` is where that is absorbed.
    MaxHeap<int>::StateSnapshot state;
    state.heap = {1, 1, 2, 3};       // 1 duplicated by the remap
    state.activity = {{1, 5.0}, {2, 7.0}}; // 3 has no activity entry

    MaxHeap<int> heap;
    heap.load_snapshot(state);
    CHECK(heap.size() == 2);
    CHECK(heap.peek_max() == 2);
    CHECK(drain(heap) == std::vector<int>{2, 1});
}

TEST_CASE("MaxHeap: works with a custom hash") {
    struct LengthHash {
        std::size_t operator()(const std::string& s) const noexcept { return s.size(); }
    };

    MaxHeap<std::string, LengthHash> heap;
    heap.set_activity("a", 1.0);
    heap.set_activity("bb", 3.0);
    heap.set_activity("cc", 2.0);
    for (const char* key : {"a", "bb", "cc"}) {
        heap.insert(key);
    }
    CHECK(heap.size() == 3);
    CHECK(heap.peek_max() == "bb");
}

} // namespace xorcle
