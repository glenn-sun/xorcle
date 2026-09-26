#pragma once

#include <cassert>
#include <cstddef>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace xorcle {

/// @brief Max-heap over integer keys, ranked by activity and then by ascending key.
///
/// Keys index the activity and heap-position tables, so they are dense and small;
/// a key never named has activity `0`. The order is total, so the maximum of a
/// set is one key whatever the order of insertions that built the heap. An
/// activity outlives the heap: a key removed by `extract_max()` keeps its score
/// for when it is inserted again.
class MaxHeap {
public:
    using Key = std::size_t;

    /// @brief Add `delta` to the activity of `key`.
    /// @pre `delta >= 0`.
    void increase_key(Key key, double delta) {
        assert(delta >= 0.0 && "increase_key requires a non-negative delta");
        set_activity(key, activity(key) + delta);
    }

    double activity(Key key) const {
        return key < activity_.size() ? activity_[key] : 0.0;
    }

    /// @brief `std::nullopt` if empty.
    std::optional<Key> extract_max() {
        if (heap_.empty()) {
            return std::nullopt;
        }
        return take_max();
    }

    /// @brief Multiply all activities by `k`.
    /// @pre `k >= 0`.
    void rescale(double k) {
        assert(k >= 0.0 && "rescale requires k >= 0");
        for (double& value : activity_) {
            value *= k;
        }
        for (Entry& entry : heap_) {
            entry.activity *= k;
        }
        // Scaling can bring two activities together, which reranks the pair.
        heapify();
    }

    void reset() noexcept {
        heap_.clear();
        key_to_heap_idx_.clear();
        activity_.clear();
    }

    /// @brief The highest-ranked key still in the heap.
    /// @pre `!empty()`.
    Key peek_max() const {
        assert(!heap_.empty() && "peek_max on empty heap");
        return heap_.front().key;
    }

    bool empty() const noexcept { return heap_.empty(); }

    /// @brief Set an activity without inserting into the heap.
    void set_activity(Key key, double value) {
        grow_for(key);

        const double old_value = activity_[key];
        activity_[key] = value;

        const std::size_t heap_idx = key_to_heap_idx_[key];
        if (heap_idx == not_in_heap) {
            return;
        }

        heap_[heap_idx].activity = value;
        if (value > old_value) {
            sift_up(heap_idx);
        } else if (value < old_value) {
            sift_down(heap_idx);
        }
    }

    /// @brief Put a key into the heap; a no-op if it is already there.
    void insert(Key key) {
        grow_for(key);
        if (key_to_heap_idx_[key] != not_in_heap) {
            return;
        }
        const std::size_t heap_idx = heap_.size();
        heap_.push_back(Entry{key, activity_[key]});
        key_to_heap_idx_[key] = heap_idx;
        sift_up(heap_idx);
    }

private:
    struct Entry {
        Key key;
        double activity;
    };

    static constexpr std::size_t not_in_heap = std::numeric_limits<std::size_t>::max();

    std::vector<Entry> heap_;                  ///< Binary heap (0-based).
    std::vector<std::size_t> key_to_heap_idx_;
    std::vector<double> activity_;

    void grow_for(Key key) {
        if (key < activity_.size()) {
            return;
        }
        activity_.resize(key + 1, 0.0);
        key_to_heap_idx_.resize(key + 1, not_in_heap);
    }

    static std::size_t parent(std::size_t i) { return (i - 1) / 2; }
    static std::size_t left(std::size_t i) { return 2 * i + 1; }
    static std::size_t right(std::size_t i) { return 2 * i + 2; }

    static bool outranks(const Entry& lhs, const Entry& rhs) {
        if (lhs.activity != rhs.activity) {
            return lhs.activity > rhs.activity;
        }
        return lhs.key < rhs.key;
    }

    Key take_max() {
        const Key top_key = heap_.front().key;
        key_to_heap_idx_[top_key] = not_in_heap;

        const std::size_t last = heap_.size() - 1;
        if (last == 0) {
            heap_.pop_back();
            return top_key;
        }

        heap_[0] = heap_[last];
        heap_.pop_back();
        key_to_heap_idx_[heap_[0].key] = 0;
        sift_down(0);
        return top_key;
    }

    void swap_positions(std::size_t a, std::size_t b) {
        if (a == b) {
            return;
        }
        std::swap(heap_[a], heap_[b]);
        key_to_heap_idx_[heap_[a].key] = a;
        key_to_heap_idx_[heap_[b].key] = b;
    }

    void sift_up(std::size_t heap_idx) {
        while (heap_idx > 0) {
            const std::size_t p = parent(heap_idx);
            if (outranks(heap_[heap_idx], heap_[p])) {
                swap_positions(heap_idx, p);
                heap_idx = p;
            } else {
                break;
            }
        }
    }

    void sift_down(std::size_t heap_idx) {
        const std::size_t n = heap_.size();
        while (true) {
            const std::size_t l = left(heap_idx);
            const std::size_t r = right(heap_idx);
            std::size_t largest = heap_idx;

            if (l < n && outranks(heap_[l], heap_[largest])) {
                largest = l;
            }
            if (r < n && outranks(heap_[r], heap_[largest])) {
                largest = r;
            }

            if (largest == heap_idx) {
                break;
            }
            swap_positions(heap_idx, largest);
            heap_idx = largest;
        }
    }

    void heapify() {
        for (std::size_t i = heap_.size() / 2; i-- > 0;) {
            sift_down(i);
        }
    }
};

} // namespace xorcle
