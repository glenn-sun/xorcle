#pragma once

/// @file progress_reporter.h
/// @warning The line shape is columnar, not `Key: value`, which is the end-of-run
/// statistics' shape.

#include <cstddef>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>

namespace xorcle {

/// @brief Which columns a progress line carries, as a bitmask.
///
/// `defaults` is what a run reports without being asked: how long, how much
/// work, how fast, how big the clause database, and how much of the search
/// space is left. The rest are for debugging one subsystem at a time.
namespace progress_group {

inline constexpr unsigned defaults = 1u << 0;
inline constexpr unsigned search = 1u << 1;
inline constexpr unsigned deletion = 1u << 2;
inline constexpr unsigned propagation = 1u << 3;
inline constexpr unsigned all = defaults | search | deletion | propagation;

} // namespace progress_group

/// @brief Parse a `--report-type=` list, e.g. `default,deletion`.
/// @return The mask, or `std::nullopt` if a name is not one of `default`,
/// `search`, `deletion`, `propagation` or `all`.
/// @note `defaults` is always included: the columns that track progress are in
/// every line, whatever else is asked for.
std::optional<unsigned> parse_progress_groups(std::string_view text);

/// @brief The accepted `--report-type=` names, comma-joined, for help and errors.
std::string progress_group_names();

/// @brief The solver state one progress line reports.
struct ProgressSnapshot {
    double time_s = 0.0;
    std::size_t num_conflicts = 0;
    /// @brief Conflicts per second over the interval since the last line, which
    /// says whether the search has slowed rather than what it has averaged.
    double conflicts_per_second = 0.0;
    std::size_t num_propagations = 0;
    std::size_t num_clauses = 0; ///< Live: deletion has already been subtracted.
    /// @brief Decision level averaged over the conflicts in the interval, sampled
    /// *at* each conflict.
    double average_decision_level = 0.0;
    double average_trail_length = 0.0; ///< As `average_decision_level`, of the trail length.
    /// @brief Active variables less the root trail's rank: the dimension of the
    /// space still to be searched.
    std::size_t remaining_vars = 0;

    std::size_t decisions = 0;
    std::size_t num_restarts = 0;

    std::size_t num_deleted_clauses = 0;

    std::size_t num_attempt_props = 0;
    std::size_t num_watch_updates = 0;
};

/// @brief Write progress as aligned DIMACS `c` comment lines, under a
/// column-name line repeated often enough to stay on screen.
class ConsoleProgressReporter {
public:
    /// @param out Not owned, so it must outlive the reporter.
    /// @param groups Which columns to write; see `progress_group`.
    explicit ConsoleProgressReporter(std::ostream& out,
                                     unsigned groups = progress_group::defaults);

    /// @brief Write one snapshot, taken once per report interval.
    void report(const ProgressSnapshot& snapshot);

private:
    /// @brief Lines between two column-name lines.
    static constexpr std::size_t header_interval = 25;

    std::ostream& out_;
    unsigned groups_;
    std::size_t lines_since_header_ = 0;
    bool header_written_ = false;

    void write_header(std::ostream& output);
};

} // namespace xorcle
