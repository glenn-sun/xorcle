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

std::size_t
learned_clause_count(const RunStats& stats) {
    return stats.final_clauses >= stats.start_clauses ? stats.final_clauses - stats.start_clauses : 0;
}

} // namespace xorcle
