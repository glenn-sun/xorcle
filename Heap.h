#include <vector>
#include <cassert>
#include <algorithm>

// This file was created with the assistance of large language models.

// A max-heap over elements of type T, keyed by an external array "key".
// Supports insert, remove(element), removeMax(), increaseKey(element), and empty().

template<typename T>
class Heap {
public:
    // Construct a heap over all elements [0 .. key.size()-1]
    // Initially all elements are in the heap.
    Heap(const std::vector<double>& key)
        : key(key)
    {
        int n = key.size();
        heap.reserve(n);
        idx.assign(n, -1);

        // initialize heap array and idx
        for (T x = 0; x < n; ++x) {
            heap.push_back(x);
            idx[x] = x;
        }
        
        // build heap
        for (int i = (n/2) - 1; i >= 0; --i)
            siftDown(i);
    }

    bool empty() const { return heap.empty(); }

    // Insert a new element x (must not already be present)
    void insert(const T& x) {
        assert(x >= 0 && x < idx.size());
        assert(idx[x] == -1);
        idx[x] = heap.size();
        heap.push_back(x);
        siftUp(idx[x]);
    }

    // Remove arbitrary element x (must be present)
    void remove(const T& x) {
        int i = idx[x];
        assert(i >= 0 && i < heap.size());
        
        int last = heap.size() - 1;
        if (i != last) {
            swapPositions(i, last);
            heap.pop_back();
            idx[x] = -1;

            // restore heap property at pos i
            siftDown(i);
            siftUp(i);
        } else {
            heap.pop_back();
            idx[x] = -1;
        }
    }

    // Pop and return the element with maximum key
    T removeMax() {
        assert(!heap.empty());
        T top = heap[0];
        remove(top);
        return top;
    }

    // Notify that the key of element x has increased (must be present)
    void increase(const T& x) {
        int i = idx[x];
        assert(i >= 0 && i < heap.size());
        siftUp(i);
    }

private:
    std::vector<T>        heap;   // binary heap array
    std::vector<int>      idx;    // idx[x] = position of x in heap, or -1 if not present
    const std::vector<double>& key;

    // Swap positions i and j in heap and update idx accordingly
    void swapPositions(int i, int j) {
        std::swap(heap[i], heap[j]);
        idx[heap[i]] = i;
        idx[heap[j]] = j;
    }

    void siftUp(int i) {
        while (i > 0) {
            int p = (i - 1) >> 1;
            if (key[heap[i]] <= key[heap[p]]) break;
            swapPositions(i, p);
            i = p;
        }
    }

    void siftDown(int i) {
        int n = heap.size();
        while (true) {
            int l = (i << 1) + 1;
            int r = l + 1;
            int largest = i;

            if (l < n && key[heap[l]] > key[heap[largest]]) largest = l;
            if (r < n && key[heap[r]] > key[heap[largest]]) largest = r;

            if (largest == i) break;
            swapPositions(i, largest);
            i = largest;
        }
    }
};