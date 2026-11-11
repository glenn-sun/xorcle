#include "util/stats.h"

#include <cassert>
#include <utility>

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
    std::unreachable();
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
    // Each condition reads the recorded configuration rather than a flag of its own.
    switch (section) {
        case StatsSection::instance:
        case StatsSection::search:
        case StatsSection::clauses:
        case StatsSection::timing:
        case StatsSection::solve_time:
            return true;
        case StatsSection::restart: {
            // A row with no recorded schedule has nothing to say about one,
            // which is not the same as a schedule named `none`.
            const std::string restart = config_value(*this, "restart");
            return (!restart.empty() && restart != "none") ||
                   config_value(*this, "stabilize") == "true";
        }
        case StatsSection::stabilize:
            return config_value(*this, "stabilize") == "true";
        case StatsSection::phase_saving: {
            const std::string phase_saving = config_value(*this, "phase_saving");
            return !phase_saving.empty() && phase_saving != "none";
        }
        case StatsSection::minimize:
            return config_value(*this, "minimize") == "true";
        case StatsSection::subsume:
            return config_value(*this, "otf_subsume") == "true" ||
                   config_value(*this, "inproc_subsume_strengthen") == "true";
        case StatsSection::vivify:
            return config_value(*this, "vivify") == "true";
        case StatsSection::subsume_pass:
            return config_value(*this, "inproc_subsume_strengthen") == "true";
        case StatsSection::deletion:
            return config_value(*this, "deletion") == "true";
        case StatsSection::propagate_budget: {
            const std::string miss_rate = config_value(*this, "propagate_miss_rate");
            return config_value(*this, "selection") == "cyclic" && !miss_rate.empty() &&
                   miss_rate != "0";
        }
        case StatsSection::bve:
            return config_value(*this, "bve") == "true";
        case StatsSection::implication_graph:
            return config_value(*this, "implication_graph") == "true";
    }
    assert(false && "RunStats::reports: not a statistics section");
    std::unreachable();
}

} // namespace xorcle
