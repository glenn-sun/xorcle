#pragma once

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
/// keeps its score and can be put back by `rebuild()`, which is what lets the
/// VSIDS-family decision procedures walk down the heap discarding candidates the
/// trail has already satisfied, then restore them for the next decision. Ties
/// are broken by heap position, so the insertion order of equal-activity
/// elements is observable in the solver's search order.
template <typename T, typename Hash = std::hash<T>, typename KeyEqual = std::equal_to<T>>
class MaxHeap {
public:
    MaxHeap() = default;

    static_assert(std::is_copy_constructible<T>::value || std::is_move_constructible<T>::value,
                  "T must be copyable or movable");

    /// @brief A negative delta does nothing. @pre Element has an activity entry.
    void increase_key(const T& element, double delta) {
        if (delta < 0.0) return;
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

    double get_priority(const T& element) const {
        auto it = activity_map_.find(element);
        if (it == activity_map_.end()) return 0.0;
        return it->second;
    }

    /// @brief `std::nullopt` if empty; the element is remembered as popped and
    /// comes back on `rebuild()`.
    std::optional<T> extract_max() {
        if (heap_.empty()) return std::nullopt;

        T top_value = std::move(heap_.front());
        pos_map_.erase(top_value);
        popped_.push_back(top_value);

        const std::size_t last = heap_.size() - 1;
        if (last == 0) {
            heap_.pop_back();
            return top_value;
        }

        heap_[0] = std::move(heap_[last]);
        heap_.pop_back();
        pos_map_[heap_[0]] = 0;
        sift_down(0);
        return top_value;
    }

    /// @brief Multiply all priorities by k > 0. No heapify: a positive constant
    /// preserves comparisons.
    void rescale(double k) {
        assert(k > 0.0 && "rescale requires k > 0");
        for (auto& entry : activity_map_) entry.second *= k;
    }

    void reset() noexcept {
        heap_.clear();
        pos_map_.clear();
        activity_map_.clear();
        popped_.clear();
    }

    const T& peek_max() const {
        assert(!heap_.empty() && "peek_max on empty heap");
        return heap_.front();
    }

    bool empty() const noexcept { return heap_.empty(); }

    /// @brief Set an activity score without inserting into the heap.
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

    void insert(const T& element) {
        if (pos_map_.find(element) != pos_map_.end()) return;
        assert(activity_map_.find(element) != activity_map_.end() &&
               "insert requires existing activity entry");
        const std::size_t idx = heap_.size();
        heap_.push_back(element);
        pos_map_.emplace(heap_.back(), idx);
        sift_up(idx);
    }

    void rebuild() {
        for (const auto& element : popped_) {
            if (pos_map_.find(element) == pos_map_.end()) {
                insert(element);
            }
        }
        popped_.clear();
    }

    /// @brief State for remapping by callers: compaction rewrites every key,
    /// which the heap cannot do itself, so the caller snapshots, remaps, reloads.
    struct StateSnapshot {
        std::vector<T> heap;                                    ///< Live elements, in heap order.
        std::vector<T> popped;                                  ///< Awaiting `rebuild()`.
        std::unordered_map<T, double, Hash, KeyEqual> activity;
    };

    StateSnapshot snapshot() const {
        StateSnapshot out;
        out.heap = heap_;
        out.popped = popped_;
        out.activity = activity_map_;
        return out;
    }

    /// @brief Replace heap/activity state. Keys without an activity entry, and
    /// duplicate keys, are dropped: remapping can collapse two distinct keys
    /// onto one, so this is expected rather than an invariant violation.
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
    std::unordered_map<T, double, Hash, KeyEqual> activity_map_;
    std::vector<T> popped_;                                      ///< Extracted since last rebuild.

    static std::size_t parent(std::size_t i) { return (i - 1) / 2; }
    static std::size_t left(std::size_t i) { return 2 * i + 1; }
    static std::size_t right(std::size_t i) { return 2 * i + 2; }

    double activity_of(const T& element) const {
        auto it = activity_map_.find(element);
        assert(it != activity_map_.end() && "activity lookup missing");
        return it->second;
    }

    void swap_positions(std::size_t a, std::size_t b) {
        if (a == b) return;
        std::swap(heap_[a], heap_[b]);
        pos_map_[heap_[a]] = a;
        pos_map_[heap_[b]] = b;
    }

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
