#include "util/RandomUtils.h"

#include <numeric>
#include <random>

namespace util {
int weighted_random_index(const std::vector<int>& weights) {
    static std::mt19937 rng(std::random_device{}());

    int total = std::accumulate(weights.begin(), weights.end(), 0);
    std::uniform_int_distribution<int> dist(0, total - 1);

    int r = dist(rng);
    int cumulative = 0;

    for (size_t i = 0; i < weights.size(); ++i) {
        cumulative += weights[i];
        if (r < cumulative) {
            return static_cast<int>(i);
        }
    }

    // Should never happen if weights are valid
    return -1;
}

int random_int(int n) {
    static std::random_device rd;
    static std::mt19937 gen(rd());  // Mersenne Twister RNG
    std::uniform_int_distribution<> dist(0, n - 1);
    return dist(gen);
}
} // namespace util
