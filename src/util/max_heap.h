#pragma once

/// @file max_heap.h
/// @brief Max-heap with activity scores for arbitrary keys.

#include <cassert>
#include <cstddef>
#include <functional>
#include <optional>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace xorcle {

/// @brief Max-heap keyed by activity values stored externally.
///
/// The activity map outlives the heap: an element removed by `extract_max()`
/// keeps its score and can be put back by `rebuild()`. That is what lets the
/// VSIDS-family decision procedures walk down the heap discarding candidates
/// that the trail has already satisfied, then restore the discarded ones for
/// the next decision.
///
/// Ties are broken by heap position, so the insertion order of equal-activity
/// elements is observable in the solver's search order.
///
/// @tparam T Element type.
/// @tparam Hash Hash functor for T. `Equation` supplies `EquationHash`; the
/// single-namespace rule forbids specializing `std::hash`.
/// @tparam KeyEqual Equality functor for T.
template <typename T, typename Hash = std::hash<T>, typename KeyEqual = std::equal_to<T>>
class MaxHeap {
public:
    /// @brief Construct an empty heap.
    MaxHeap() = default;

    /// @brief Require T to be hashable and copyable or movable.
    static_assert(std::is_copy_constructible<T>::value || std::is_move_constructible<T>::value,
                  "T must be copyable or movable");

    /// @brief Increase activity by delta.
    /// @pre Element already has an activity entry.
    /// @note If delta < 0 this does nothing.
    void increase_key(const T& element, double delta) {
        if (delta < 0.0) return; // nothing to do for non-positive delta
        auto act_it = activity_map_.find(element);
        assert(act_it != activity_map_.end() && "increase_key requires existing activity entry");

        const double old_value = act_it->second;
        act_it->second = old_value + delta;

        auto pos_it = pos_map_.find(element);
        if (pos_it == pos_map_.end()) return;

        const std::size_t pos = pos_it->second;
        if (act_it->second > old_value) {
            sift_up(pos);
        } else if (act_it->second < old_value) {
            sift_down(pos);
        }
    }

    /// @brief Return current priority for element; if not present, return 0.0.
    double get_priority(const T& element) const {
        auto it = activity_map_.find(element);
        if (it == activity_map_.end()) return 0.0;
        return it->second;
    }

    /// @brief Extract and return the element with maximum priority.
    /// @return The max element, or `std::nullopt` if the heap is empty.
    /// @note The element is remembered as popped and returns on `rebuild()`.
    std::optional<T> extract_max() {
        if (heap_.empty()) return std::nullopt;

        T top_value = std::move(heap_.front()); // take the element to return
        // remove from map (we will remove the root)
        pos_map_.erase(top_value);
        popped_.push_back(top_value);

        const std::size_t last = heap_.size() - 1;
        if (last == 0) {
            heap_.pop_back();
            return top_value;
        }

        // Move last node to root position
        heap_[0] = std::move(heap_[last]);
        heap_.pop_back();

        // update pos_map_ for moved element
        pos_map_[heap_[0]] = 0;

        // restore heap
        sift_down(0);
        return top_value;
    }

    /// @brief Multiply (rescale) all priorities by k.
    /// @pre k > 0.
    void rescale(double k) {
        assert(k > 0.0 && "rescale requires k > 0");
        for (auto& entry : activity_map_) entry.second *= k;
        // Multiplying by a positive constant preserves comparisons; no heapify needed.
    }

    /// @brief Reset the heap, deleting everything.
    void reset() noexcept {
        heap_.clear();
        pos_map_.clear();
        activity_map_.clear();
        popped_.clear();
    }

    /// @brief Peek at max element without removing.
    /// @pre Heap is not empty.
    const T& peek_max() const {
        assert(!heap_.empty() && "peek_max on empty heap");
        return heap_.front();
    }

    /// @brief Number of elements currently in heap.
    std::size_t size() const noexcept { return heap_.size(); }

    /// @brief Return true if the heap is empty.
    bool empty() const noexcept { return heap_.empty(); }

    /// @brief Register or overwrite an activity score without inserting into the heap.
    void set_activity(const T& element, double value) {
        double old_value = 0.0;
        bool had_entry = false;
        auto it = activity_map_.find(element);
        if (it != activity_map_.end()) {
            old_value = it->second;
            had_entry = true;
            it->second = value;
        } else {
            activity_map_.emplace(element, value);
        }

        auto pos_it = pos_map_.find(element);
        if (pos_it == pos_map_.end()) return;

        const std::size_t pos = pos_it->second;
        if (!had_entry || value > old_value) {
            sift_up(pos);
        } else if (value < old_value) {
            sift_down(pos);
        }
    }

    /// @brief Insert an element into the heap using its existing activity score.
    /// @pre Element already has an activity entry.
    void insert(const T& element) {
        if (pos_map_.find(element) != pos_map_.end()) return;
        assert(activity_map_.find(element) != activity_map_.end() &&
               "insert requires existing activity entry");
        const std::size_t idx = heap_.size();
        heap_.push_back(element);
        pos_map_.emplace(heap_.back(), idx);
        sift_up(idx);
    }

    /// @brief Reinsert all popped elements into the heap.
    void rebuild() {
        for (const auto& element : popped_) {
            if (pos_map_.find(element) == pos_map_.end()) {
                insert(element);
            }
        }
        popped_.clear();
    }

    /// @brief Snapshot of internal state for remapping/rebuilding by callers.
    ///
    /// Compaction rewrites every key, which the heap cannot do itself; the
    /// caller takes a snapshot, remaps the keys, and loads it back.
    struct StateSnapshot {
        std::vector<T> heap;                                    ///< Live elements, in heap order.
        std::vector<T> popped;                                  ///< Elements awaiting `rebuild()`.
        std::unordered_map<T, double, Hash, KeyEqual> activity;  ///< Element -> activity.
    };

    /// @brief Return a copy of the current heap/activity state.
    StateSnapshot snapshot() const {
        StateSnapshot out;
        out.heap = heap_;
        out.popped = popped_;
        out.activity = activity_map_;
        return out;
    }

    /// @brief Replace heap/activity state; validates that heap keys have activities.
    /// @note Keys without an activity entry, and duplicate keys, are dropped.
    /// Remapping can collapse two distinct keys onto one, so this is expected
    /// rather than an invariant violation.
    void load_snapshot(StateSnapshot state) {
        heap_.clear();
        popped_.clear();
        pos_map_.clear();
        activity_map_ = std::move(state.activity);
        popped_ = std::move(state.popped);

        heap_.reserve(state.heap.size());
        for (const T& key : state.heap) {
            if (activity_map_.find(key) == activity_map_.end()) {
                continue;
            }
            if (pos_map_.find(key) != pos_map_.end()) {
                continue;
            }
            pos_map_[key] = heap_.size();
            heap_.push_back(key);
        }
        if (!heap_.empty()) {
            for (std::size_t i = heap_.size() / 2; i-- > 0;) {
                sift_down(i);
            }
        }
    }

private:
    std::vector<T> heap_;                                        ///< Binary heap (0-based).
    std::unordered_map<T, std::size_t, Hash, KeyEqual> pos_map_; ///< Element -> index in heap_.
    std::unordered_map<T, double, Hash, KeyEqual> activity_map_; ///< Element -> activity.
    std::vector<T> popped_;                                      ///< Elements extracted since last rebuild.

    /// @brief Return parent index in the heap.
    static std::size_t parent(std::size_t i) { return (i - 1) / 2; }

    /// @brief Return left child index in the heap.
    static std::size_t left(std::size_t i) { return 2 * i + 1; }

    /// @brief Return right child index in the heap.
    static std::size_t right(std::size_t i) { return 2 * i + 2; }

    /// @brief Return the activity value for an element.
    /// @pre Element must exist in activity_map_.
    double activity_of(const T& element) const {
        auto it = activity_map_.find(element);
        assert(it != activity_map_.end() && "activity lookup missing");
        return it->second;
    }

    /// @brief Swap two positions in heap (indices are positions in heap_).
    void swap_positions(std::size_t a, std::size_t b) {
        if (a == b) return;
        std::swap(heap_[a], heap_[b]);
        pos_map_[heap_[a]] = a;
        pos_map_[heap_[b]] = b;
    }

    /// @brief Sift-up from position pos (restore max-heap property going up).
    void sift_up(std::size_t pos) {
        while (pos > 0) {
            const std::size_t p = parent(pos);
            if (activity_of(heap_[pos]) > activity_of(heap_[p])) {
                swap_positions(pos, p);
                pos = p;
            } else {
                break;
            }
        }
    }

    /// @brief Sift-down from position pos (restore max-heap property going down).
    void sift_down(std::size_t pos) {
        const std::size_t n = heap_.size();
        while (true) {
            const std::size_t l = left(pos);
            const std::size_t r = right(pos);
            std::size_t largest = pos;

            if (l < n && activity_of(heap_[l]) > activity_of(heap_[largest])) largest = l;
            if (r < n && activity_of(heap_[r]) > activity_of(heap_[largest])) largest = r;

            if (largest != pos) {
                swap_positions(pos, largest);
                pos = largest;
            } else {
                break;
            }
        }
    }
};

} // namespace xorcle
