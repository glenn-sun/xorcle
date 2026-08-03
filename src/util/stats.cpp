#include "util/stats.h"

namespace xorcle {

std::string
to_string(const SolveResult result) {
    switch (result) {
        case SolveResult::sat:
            return "SAT";
        case SolveResult::unsat:
            return "UNSAT";
    }
    return "unknown";
}

std::size_t
learned_clause_count(const RunStats& stats) {
    return stats.final_clauses >= stats.start_clauses ? stats.final_clauses - stats.start_clauses : 0;
}

} // namespace xorcle
