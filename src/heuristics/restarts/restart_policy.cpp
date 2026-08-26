#include "heuristics/restarts/restart_policy.h"

#include <cassert>

#include "heuristics/restarts/lbd.h"
#include "heuristics/restarts/luby.h"

namespace xorcle {

bool
NoRestarts::enabled() const {
    return false;
}

bool
NoRestarts::should_restart_after_conflict(const ConflictSummary&) {
    return false;
}

void
NoRestarts::note_restart() {}

std::unique_ptr<RestartPolicy>
make_restart_policy(const RestartConfig& config) {
    switch (config.method) {
        case RestartMethod::none:
            return std::make_unique<NoRestarts>();
        case RestartMethod::luby:
            return std::make_unique<Luby>(config);
        case RestartMethod::lbd:
            return std::make_unique<Lbd>(config);
    }
    assert(false && "unhandled restart method");
    return std::make_unique<NoRestarts>();
}

} // namespace xorcle
