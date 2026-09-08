#include "util/progress_reporter.h"

#include <array>
#include <iomanip>
#include <iterator>
#include <ostream>
#include <sstream>
#include <string>
#include <tuple>

namespace xorcle {

namespace {

struct Column {
    const char* name;
    int width;
};

/// @brief The columns, in print order; the header and every line share them.
constexpr Column columns[]{
    {"seconds", 9},          {"conflicts", 12},   {"propagations", 15}, {"restarts", 10},
    {"decisions", 12},       {"decision level", 17}, {"trail rank", 13}, {"remaining vars", 17},
    {"deletion passes", 18}, {"deleted", 10},     {"demoted", 10},      {"promoted", 11},
};

std::string format_seconds(const double time_s) {
    std::ostringstream text;
    text << std::fixed << std::setprecision(2) << time_s;
    return text.str();
}

} // namespace

ConsoleProgressReporter::ConsoleProgressReporter(std::ostream& out) : out_(out) {}

void
ConsoleProgressReporter::report(const ProgressSnapshot& snapshot) {
    std::ostringstream output;

    if (!header_written_) {
        output << "c";
        for (const Column& column : columns) {
            output << std::setw(column.width) << column.name;
        }
        output << "\n";
        header_written_ = true;
    }

    const std::array cells{
        format_seconds(snapshot.time_s),
        std::to_string(snapshot.num_conflicts),
        std::to_string(snapshot.num_propagations),
        std::to_string(snapshot.num_restarts),
        std::to_string(snapshot.decisions),
        std::to_string(snapshot.decision_level),
        std::to_string(snapshot.trail_rank),
        std::to_string(snapshot.remaining_vars),
        std::to_string(snapshot.num_deletion_passes),
        std::to_string(snapshot.num_deleted_clauses),
        std::to_string(snapshot.num_demoted_clauses),
        std::to_string(snapshot.num_promoted_clauses),
    };
    static_assert(std::tuple_size_v<decltype(cells)> == std::size(columns));

    output << "c";
    for (std::size_t index = 0; index < cells.size(); ++index) {
        output << std::setw(columns[index].width) << cells[index];
    }
    output << "\n";

    out_ << output.str() << std::flush;
}

} // namespace xorcle
