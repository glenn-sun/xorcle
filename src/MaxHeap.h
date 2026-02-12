#pragma once

// This file was generated with the assistance of AI.

#include <vector>
#include <unordered_map>
#include <stdexcept>
#include <cstddef>
#include <utility>
#include <type_traits>
#include <optional>
#include <queue>
#include <functional> 

template <
    typename T,
    typename Hash = std::hash<T>,
    typename KeyEqual = std::equal_to<T>
>
class MaxHeap {
public:
    // Construct empty heap
    MaxHeap() = default;

    // Non-copyable/non-movable map keys may still be copied into nodes; require T to be hashable and copyable/movable.
    static_assert(std::is_copy_constructible<T>::value || std::is_move_constructible<T>::value,
                  "T must be copyable or movable");

    // Increase activity by delta. Element must already have an activity entry.
    // If delta < 0 this does nothing.
    void increase_key(const T& element, double delta) {
        if (delta < 0.0) return; // nothing to do for non-positive delta
        auto act_it = activity_map_.find(element);
        if (act_it == activity_map_.end()) {
            throw std::runtime_error("increase_key requires existing activity entry");
        }

        double old_value = act_it->second;
        act_it->second = old_value + delta;

        auto pos_it = pos_map_.find(element);
        if (pos_it == pos_map_.end()) return;

        std::size_t pos = pos_it->second;
        if (act_it->second > old_value) {
            sift_up(pos);
        } else if (act_it->second < old_value) {
            sift_down(pos);
        }
    }

    // Remove all entries with priority strictly less than cutoff.
    // This operation rebuilds the internal heap and updates the position map.
    void remove_below(double cutoff) {
        if (heap_.empty()) return;

        std::vector<T> new_heap;
        new_heap.reserve(heap_.size());

        // Move kept nodes into new_heap; erase removed nodes from pos_map_
        for (std::size_t i = 0; i < heap_.size(); ++i) {
            if (activity_of(heap_[i]) < cutoff) {
                pos_map_.erase(heap_[i]);
            } else {
                new_heap.push_back(std::move(heap_[i]));
            }
        }

        heap_.swap(new_heap);

        // Rebuild pos_map_ for the compacted heap
        pos_map_.clear();
        for (std::size_t i = 0; i < heap_.size(); ++i) {
            pos_map_.emplace(heap_[i], i);
        }

        // Heapify in O(n) by sifting down from the last non-leaf node.
        if (!heap_.empty()) {
            for (std::size_t i = heap_.size() / 2; i-- > 0; ) {
                sift_down(i);
            }
        }
    }


    // Return current priority for element; if not present, return 0.0
    double get_priority(const T& element) const {
        auto it = activity_map_.find(element);
        if (it == activity_map_.end()) return 0.0;
        return it->second;
    }

    // Extract and return the element with maximum priority.
    // Throws std::runtime_error if called on empty heap.
    std::optional<T> extract_max() {
        if (heap_.empty()) return std::nullopt;
        // if (heap_.empty()) throw std::runtime_error("extract_max called on empty heap");

        T top_value = std::move(heap_.front()); // take the element to return
        // remove from map (we will remove the root)
        pos_map_.erase(top_value);
        popped_.push_back(top_value);

        std::size_t last = heap_.size() - 1;
        if (last == 0) {
            heap_.pop_back();
            return top_value;
        }

        // Move last node to root position
        heap_[0] = std::move(heap_[last]);
        heap_.pop_back();

        // update pos_map_ for moved element
        pos_map_[ heap_[0] ] = 0;

        // restore heap
        sift_down(0);
        return top_value;
    }

    // Non-destructively peek the top `k` elements (1..k largest).
    // Returns a vector of const references to the heap elements in descending priority order.
    std::vector<std::reference_wrapper<const T>> peek_top_k(std::size_t k) const {
        if (k == 0) return {};
        k = std::min(k, heap_.size());

        using Cand = std::pair<double, std::size_t>; // (priority, index)
        auto cmp = [](const Cand &a, const Cand &b) { return a.first < b.first; }; // max-first
        std::priority_queue<Cand, std::vector<Cand>, decltype(cmp)> pq(cmp);

        pq.emplace(activity_of(heap_[0]), 0);

        std::vector<std::reference_wrapper<const T>> result;
        result.reserve(k);

        while (!pq.empty() && result.size() < k) {
            auto [priority, idx] = pq.top();
            pq.pop();

            result.emplace_back(std::cref(heap_[idx]));

            std::size_t l = left(idx);
            std::size_t r = right(idx);
            if (l < heap_.size()) pq.emplace(activity_of(heap_[l]), l);
            if (r < heap_.size()) pq.emplace(activity_of(heap_[r]), r);
        }

        return result;
    }


    // Multiply (rescale) all priorities by k. Requires k > 0 to preserve relative order.
    // Throws std::invalid_argument if k <= 0.
    void rescale(double k) {
        if (!(k > 0.0)) throw std::invalid_argument("rescale requires k > 0");
        for (auto &entry : activity_map_) entry.second *= k;
        // Multiplying by a positive constant preserves comparisons; no heapify needed.
    }

    // Reset the heap, deleting everything.
    void reset() noexcept {
        heap_.clear();
        pos_map_.clear();
        activity_map_.clear();
        popped_.clear();
    }

    // Optional convenience: peek at max element without removing. Throws if empty.
    const T& peek_max() const {
        if (heap_.empty()) throw std::runtime_error("peek_max on empty heap");
        return heap_.front();
    }

    // Number of elements currently in heap
    std::size_t size() const noexcept { return heap_.size(); }
    bool empty() const noexcept { return heap_.empty(); }

    // Register or overwrite an activity score without inserting into the heap.
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

        std::size_t pos = pos_it->second;
        if (!had_entry || value > old_value) {
            sift_up(pos);
        } else if (value < old_value) {
            sift_down(pos);
        }
    }

    // Insert an element into the heap using its existing activity score.
    void insert(const T& element) {
        if (pos_map_.find(element) != pos_map_.end()) return;
        if (activity_map_.find(element) == activity_map_.end()) {
            throw std::runtime_error("insert requires existing activity entry");
        }
        std::size_t idx = heap_.size();
        heap_.push_back(element);
        pos_map_.emplace(heap_.back(), idx);
        sift_up(idx);
    }

    // Reinsert all popped elements into the heap.
    void rebuild() {
        for (const auto& element : popped_) {
            if (pos_map_.find(element) == pos_map_.end()) {
                insert(element);
            }
        }
        popped_.clear();
    }

private:
    std::vector<T> heap_;                                      // binary heap (0-based)
    std::unordered_map<T, std::size_t, Hash, KeyEqual> pos_map_; // element -> index in heap_
    std::unordered_map<T, double, Hash, KeyEqual> activity_map_; // element -> activity
    std::vector<T> popped_;

    // Heap index helpers
    static std::size_t parent(std::size_t i) { return (i - 1) / 2; }
    static std::size_t left(std::size_t i)   { return 2 * i + 1; }
    static std::size_t right(std::size_t i)  { return 2 * i + 2; }

    double activity_of(const T& element) const {
        auto it = activity_map_.find(element);
        if (it == activity_map_.end()) {
            throw std::runtime_error("activity lookup missing");
        }
        return it->second;
    }

    // swap two positions in heap (indices are positions in heap_)
    void swap_positions(std::size_t a, std::size_t b) {
        if (a == b) return;
        std::swap(heap_[a], heap_[b]);
        pos_map_[ heap_[a] ] = a;
        pos_map_[ heap_[b] ] = b;
    }

    // sift-up from position pos (restore max-heap property going up)
    void sift_up(std::size_t pos) {
        while (pos > 0) {
            std::size_t p = parent(pos);
            if (activity_of(heap_[pos]) > activity_of(heap_[p])) {
                swap_positions(pos, p);
                pos = p;
            } else break;
        }
    }

    // sift-down from position pos (restore max-heap property going down)
    void sift_down(std::size_t pos) {
        std::size_t n = heap_.size();
        while (true) {
            std::size_t l = left(pos);
            std::size_t r = right(pos);
            std::size_t largest = pos;

            if (l < n && activity_of(heap_[l]) > activity_of(heap_[largest])) largest = l;
            if (r < n && activity_of(heap_[r]) > activity_of(heap_[largest])) largest = r;

            if (largest != pos) {
                swap_positions(pos, largest);
                pos = largest;
            } else break;
        }
    }
};
