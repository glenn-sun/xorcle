#include "util/RandomUtils.h"

#include <random>

namespace util {
int random_int(int n) {
    static std::random_device rd;
    static std::mt19937 gen(rd());  // Mersenne Twister RNG
    std::uniform_int_distribution<> dist(0, n - 1);
    return dist(gen);
}
} // namespace util
