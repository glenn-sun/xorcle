#pragma once

/// @file ema.h
/// @brief An exponential moving average.

#include <cassert>
#include <cstddef>

namespace xorcle {

/// @brief An exponential moving average over a window, corrected for the weight
/// the zero it starts at still holds while the window fills.
///
/// Without the correction a slow average reads far below its samples for as
/// many updates as its window is wide.
class Ema {
public:
    /// @param window The number of samples smoothed over; the decay is its reciprocal.
    /// @pre `window != 0`.
    explicit Ema(std::size_t window) : alpha_(1.0 / static_cast<double>(window)) {
        assert(window != 0 && "an EMA window must be non-zero");
    }

    void update(double sample) {
        raw_ += alpha_ * (sample - raw_);
        initial_weight_ *= 1.0 - alpha_;
    }

    /// @brief Forget every sample so far.
    void reset() {
        raw_ = 0.0;
        initial_weight_ = 1.0;
    }

    /// @return `0` before the first update, and the first sample exactly after it.
    double value() const {
        const double sampled_weight = 1.0 - initial_weight_;
        return sampled_weight == 0.0 ? 0.0 : raw_ / sampled_weight;
    }

private:
    double alpha_;
    double raw_ = 0.0;
    double initial_weight_ = 1.0;
};

} // namespace xorcle
