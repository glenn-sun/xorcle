#pragma once

/// @file progress_reporter.h
/// @warning The line shape is columnar, not `Key: value`.

#include <cstddef>
#include <iosfwd>

namespace xorcle {

/// @brief The solver state one progress line reports.
struct ProgressSnapshot {
    double time_s = 0.0;
    std::size_t num_conflicts = 0;
    std::size_t num_propagations = 0;
    std::size_t num_restarts = 0;
    std::size_t decisions = 0;
    std::size_t decision_level = 0;
    std::size_t trail_rank = 0;
    /// @brief Active variables less the root trail's rank: the dimension of the
    /// space still to be searched.
    std::size_t remaining_vars = 0;
    std::size_t num_deletion_passes = 0;
    std::size_t num_deleted_clauses = 0;
    std::size_t num_demoted_clauses = 0;
    std::size_t num_promoted_clauses = 0;
};

/// @brief Receives one snapshot per report interval while the solver runs.
class ProgressReporter {
public:
    virtual ~ProgressReporter() = default;

    virtual void report(const ProgressSnapshot& snapshot) = 0;
};

/// @brief Write progress as aligned DIMACS `c` comment lines, headed by one
/// column-name line before the first.
class ConsoleProgressReporter : public ProgressReporter {
public:
    /// @param out Not owned, so it must outlive the reporter.
    explicit ConsoleProgressReporter(std::ostream& out);

    void report(const ProgressSnapshot& snapshot) override;

private:
    std::ostream& out_;
    bool header_written_ = false;
};

} // namespace xorcle
