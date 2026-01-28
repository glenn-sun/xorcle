#pragma once

// This file was generated with the assistance of AI.

#include <vector>
#include <unordered_map>
#include <stdexcept>
#include <cstddef>
#include <utility>
#include <type_traits>
#include <optional>

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

    // Insert by increasing by delta (treat missing element as priority 0).
    // If delta <= 0 this does nothing.
    void increase_key(const T& element, double delta) {
        if (delta < 0.0) return; // nothing to do for non-positive delta

        auto it = pos_map_.find(element);
        if (it == pos_map_.end()) {
            // insert new node with priority = delta (initially 0 then +delta)
            std::size_t idx = heap_.size();
            heap_.push_back(Node{element, delta});
            pos_map_.emplace(heap_.back().value, idx);
            sift_up(idx);
        } else {
            std::size_t pos = it->second;
            heap_[pos].priority += delta;
            sift_up(pos);
        }
    }

    // Remove all entries with priority strictly less than cutoff.
    // This operation rebuilds the internal heap and updates the position map.
    void remove_below(double cutoff) {
        if (heap_.empty()) return;

        std::vector<Node> new_heap;
        new_heap.reserve(heap_.size());

        // Move kept nodes into new_heap; erase removed nodes from pos_map_
        for (std::size_t i = 0; i < heap_.size(); ++i) {
            if (heap_[i].priority < cutoff) {
                pos_map_.erase(heap_[i].value);
            } else {
                new_heap.push_back(std::move(heap_[i]));
            }
        }

        heap_.swap(new_heap);

        // Rebuild pos_map_ for the compacted heap
        pos_map_.clear();
        for (std::size_t i = 0; i < heap_.size(); ++i) {
            pos_map_.emplace(heap_[i].value, i);
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
        auto it = pos_map_.find(element);
        if (it == pos_map_.end()) return 0.0;
        return heap_[it->second].priority;
    }

    // Extract and return the element with maximum priority.
    // Throws std::runtime_error if called on empty heap.
    std::optional<T> extract_max() {
        if (heap_.empty()) return std::nullopt;
        // if (heap_.empty()) throw std::runtime_error("extract_max called on empty heap");

        T top_value = std::move(heap_.front().value); // take the element to return
        // remove from map (we will remove the root)
        pos_map_.erase(top_value);

        std::size_t last = heap_.size() - 1;
        if (last == 0) {
            heap_.pop_back();
            return top_value;
        }

        // Move last node to root position
        heap_[0] = std::move(heap_[last]);
        heap_.pop_back();

        // update pos_map_ for moved element
        pos_map_[ heap_[0].value ] = 0;

        // restore heap
        sift_down(0);
        return top_value;
    }

    // Multiply (rescale) all priorities by k. Requires k > 0 to preserve relative order.
    // Throws std::invalid_argument if k <= 0.
    void rescale(double k) {
        if (!(k > 0.0)) throw std::invalid_argument("rescale requires k > 0");
        for (auto &n : heap_) n.priority *= k;
        // Multiplying by a positive constant preserves comparisons; no heapify needed.
    }

    // Reset the heap, deleting everything.
    void reset() noexcept {
        heap_.clear();
        pos_map_.clear();
    }

    // Optional convenience: peek at max element without removing. Throws if empty.
    const T& peek_max() const {
        if (heap_.empty()) throw std::runtime_error("peek_max on empty heap");
        return heap_.front().value;
    }

    // Number of elements currently in heap
    std::size_t size() const noexcept { return heap_.size(); }
    bool empty() const noexcept { return heap_.empty(); }

private:
    struct Node {
        T value;
        double priority;
    };

    std::vector<Node> heap_;                             // binary heap (0-based)
    std::unordered_map<T, std::size_t, Hash, KeyEqual> pos_map_; // element -> index in heap_

    // Heap index helpers
    static std::size_t parent(std::size_t i) { return (i - 1) / 2; }
    static std::size_t left(std::size_t i)   { return 2 * i + 1; }
    static std::size_t right(std::size_t i)  { return 2 * i + 2; }

    // swap two positions in heap (indices are positions in heap_)
    void swap_positions(std::size_t a, std::size_t b) {
        if (a == b) return;
        std::swap(heap_[a], heap_[b]);
        pos_map_[ heap_[a].value ] = a;
        pos_map_[ heap_[b].value ] = b;
    }

    // sift-up from position pos (restore max-heap property going up)
    void sift_up(std::size_t pos) {
        while (pos > 0) {
            std::size_t p = parent(pos);
            if (heap_[pos].priority > heap_[p].priority) {
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

            if (l < n && heap_[l].priority > heap_[largest].priority) largest = l;
            if (r < n && heap_[r].priority > heap_[largest].priority) largest = r;

            if (largest != pos) {
                swap_positions(pos, largest);
                pos = largest;
            } else break;
        }
    }
};
