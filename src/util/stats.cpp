#include "util/stats.h"

#include <cassert>

namespace xorcle {

std::string
to_string(const SolveResult result) {
    switch (result) {
        case SolveResult::sat:
            return "SAT";
        case SolveResult::unsat:
            return "UNSAT";
    }
    assert(false && "to_string: not a solve result");
    return "unknown";
}

std::string
config_value(const RunStats& stats, const std::string_view name) {
    for (const ConfigEntry& entry : stats.config) {
        if (entry.name == name) {
            return entry.value;
        }
    }
    return {};
}

bool
RunStats::reports(const StatsSection section) const {
    // Each condition reads the recorded configuration rather than a flag of its
    // own, so the block can never claim a feature ran that the row says was off.
    switch (section) {
        case StatsSection::instance:
        case StatsSection::search:
        case StatsSection::clauses:
        case StatsSection::timing:
            return true;
        case StatsSection::restart: {
            // A row with no recorded schedule has nothing to say about one,
            // which is not the same as a schedule named `none`.
            const std::string restart = config_value(*this, "restart");
            return !restart.empty() && restart != "none";
        }
        case StatsSection::minimize:
            return config_value(*this, "minimize") == "true";
        case StatsSection::subsume:
            return config_value(*this, "subsume") == "true";
        case StatsSection::deletion:
            return config_value(*this, "deletion") == "true";
        case StatsSection::proof:
            return proof_postprocess_time_s > 0.0;
    }
    assert(false && "RunStats::reports: not a statistics section");
    return false;
}

} // namespace xorcle
