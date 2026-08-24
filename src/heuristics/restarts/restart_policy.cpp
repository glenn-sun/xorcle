#include "heuristics/restarts/restart_policy.h"

#include <cassert>

#include "heuristics/restarts/geometric.h"
#include "heuristics/restarts/luby.h"

namespace xorcle {

bool
NoRestarts::enabled() const {
    return false;
}

bool
NoRestarts::should_restart_after_conflict() {
    return false;
}

void
NoRestarts::note_restart() {}

std::unique_ptr<RestartPolicy>
make_restart_policy(const RestartConfig& config) {
    switch (config.method) {
        case RestartMethod::none:
            return std::make_unique<NoRestarts>();
        case RestartMethod::geometric:
            return std::make_unique<Geometric>(config);
        case RestartMethod::luby:
            return std::make_unique<Luby>(config);
    }
    assert(false && "unhandled restart method");
    return std::make_unique<NoRestarts>();
}

} // namespace xorcle
