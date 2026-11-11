#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "util/index_set.h"

namespace xorcle {

enum class SolveResult {
    unsat,
    sat
};

/// @brief Which part of the run a statistic belongs to.
///
/// `restart`, `stabilize`, `phase_saving`, `minimize`, `subsume`, `vivify`, `subsume_pass`, `deletion`,
/// `propagate_budget`, `bve` and `implication_graph` print only when the run had
/// something to say; `RunStats::reports` decides. The CSV writes every field whatever the section
/// says, so its schema does not move with the options.
///
/// @warning `ConsoleStatsWriter::log` names the sections one by one, in
/// this order, so that the configuration block can sit between the first two. A
/// section added here needs a call added there or it prints nowhere.
///
/// `solve_time` prints under a heading, each row with its share of the solve time.
enum class StatsSection {
    instance,
    search,
    restart,
    stabilize,
    clauses,
    phase_saving,
    minimize,
    subsume,
    vivify,
    subsume_pass,
    deletion,
    propagate_budget,
    bve,
    implication_graph,
    timing,
    solve_time
};

/// @brief The value type a `XORCLE_STAT` kind tag stands for.
struct StatKind {
    struct Count {};
    struct Real {};
};

template <typename Kind>
struct StatValue;
template <>
struct StatValue<StatKind::Count> {
    using type = std::size_t;
};
template <>
struct StatValue<StatKind::Real> {
    using type = double;
};

/// @brief One option's effective value, as the statistics report it.
struct ConfigEntry {
    std::string name;  ///< The option's table name, e.g. `lbd_window`.
    std::string value; ///< Its value as text, e.g. `16`.
    /// @brief Whether the value is the table's own default, which the console
    /// block leaves out.
    bool is_default = true;
};

/// @brief Per-run solver statistics and configuration metadata.
///
/// The numeric members come from `stats_fields.inc`; `config` carries every
/// command-line option, so a row records the whole recipe the run used.
struct RunStats {
    std::string run_id;
    std::string timestamp_utc;
    std::string file;

    /// @brief Every option and its effective value, in `options.inc` order.
    std::vector<ConfigEntry> config;

    SolveResult result = SolveResult::unsat;

    /// @brief The satisfying assignment, indexed by variable in the original
    /// numbering; set only on SAT.
    std::optional<IndexSet> sat_assignment;

#define XORCLE_STAT(section, kind, name, csv_name, label)                                          \
    StatValue<StatKind::kind>::type name = {};
#include "util/stats_fields.inc"

    /// @brief Whether the run had anything to report from @p section.
    ///
    /// The console block skips a section this rules out; the CSV writes every
    /// column regardless, so its schema does not depend on the configuration.
    bool reports(StatsSection section) const;
};

/// @brief `SAT`/`UNSAT`, the CSV spelling.
///
/// `ConsoleStatsWriter` writes `SATISFIABLE`/`UNSATISFIABLE` and does not go
/// through this.
std::string to_string(SolveResult result);

/// @brief The value of @p name in @p config, or empty when it is not there.
std::string config_value(const RunStats& stats, std::string_view name);

} // namespace xorcle
