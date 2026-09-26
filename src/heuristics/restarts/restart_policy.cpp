#include "heuristics/restarts/restart_policy.h"

#include <cassert>
#include <utility>

#include "heuristics/restarts/lbd_restarts.h"
#include "heuristics/restarts/luby.h"

namespace xorcle {

bool
NoRestarts::should_restart_after_conflict(const ConflictSummary&) {
    return false;
}

void
NoRestarts::note_restart() {}

void
RestartPolicy::add_to_stats(RunStats&) const {}

std::unique_ptr<RestartPolicy>
make_restart_policy(const RestartConfig& config) {
    switch (config.method) {
        case RestartMethod::none:
            return std::make_unique<NoRestarts>();
        case RestartMethod::luby:
            return std::make_unique<Luby>(config);
        case RestartMethod::lbd:
            return std::make_unique<LbdRestarts>(config);
    }
    assert(false && "unhandled restart method");
    std::unreachable();
}

} // namespace xorcle
