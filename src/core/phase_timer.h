#pragma once

#include <array>
#include <chrono>
#include <cstddef>

namespace xorcle {

/// @brief The parts the statistics break solve time into.
enum class SolverPhase : std::size_t {
    other,
    decisions,
    propagation,
    analysis,
    implication_graph,
    vivification,
    inproc_subsume_strengthen,
    bve,
    substitution,
    deletion,
    replay,
    count
};

/// @brief Charges wall time to one solver phase at a time.
///
/// Each switch reads the clock once and charges the time since the last one to
/// the phase it leaves, so a phase entered from inside another is not counted
/// in both.
class PhaseTimer {
public:
    /// @brief Starts charging to `other`.
    void start() {
        current_ = SolverPhase::other;
        since_ = std::chrono::steady_clock::now();
    }

    /// @brief Charges the time since the last switch and enters @p phase.
    /// @return The phase it left.
    SolverPhase switch_to(const SolverPhase phase) {
        const SolverPhase left = current_;
        if (phase != left) {
            charge();
            current_ = phase;
        }
        return left;
    }

    /// @brief Charges the time since the last switch, ending the timing.
    void stop() { charge(); }

    /// @brief The time charged to @p phase up to the last switch, in seconds.
    double seconds(const SolverPhase phase) const {
        return std::chrono::duration<double>(elapsed_[static_cast<std::size_t>(phase)]).count();
    }

private:
    void charge() {
        const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
        elapsed_[static_cast<std::size_t>(current_)] += now - since_;
        since_ = now;
    }

    std::array<std::chrono::steady_clock::duration, static_cast<std::size_t>(SolverPhase::count)>
        elapsed_{};
    SolverPhase current_ = SolverPhase::other;
    std::chrono::steady_clock::time_point since_;
};

} // namespace xorcle
