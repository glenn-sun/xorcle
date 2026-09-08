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
    unsigned group;
};

/// @brief The columns, in print order; the header and every line share them.
constexpr Column columns[]{
    {"seconds", 9, progress_group::defaults},
    {"conflicts", 12, progress_group::defaults},
    {"confl/s", 10, progress_group::defaults},
    {"propagations", 15, progress_group::defaults},
    {"clauses", 10, progress_group::defaults},
    {"avg level", 12, progress_group::defaults},
    {"avg rank", 11, progress_group::defaults},
    {"remaining vars", 17, progress_group::defaults},

    {"decisions", 12, progress_group::search},
    {"trail level", 14, progress_group::search},
    {"trail rank", 13, progress_group::search},
    {"restarts", 10, progress_group::search},
    {"reused levels", 16, progress_group::search},

    {"deleted", 10, progress_group::deletion},
    {"demoted", 10, progress_group::deletion},
    {"promoted", 11, progress_group::deletion},

    {"attempts", 13, progress_group::propagation},
    {"watch updates", 16, progress_group::propagation},
};

std::string format_fixed(const double value, const int precision) {
    std::ostringstream text;
    text << std::fixed << std::setprecision(precision) << value;
    return text.str();
}

/// @brief Every column's cell, in the order `columns` lists them.
std::array<std::string, std::size(columns)> cells_of(const ProgressSnapshot& snapshot) {
    return {
        format_fixed(snapshot.time_s, 2),
        std::to_string(snapshot.num_conflicts),
        format_fixed(snapshot.conflicts_per_second, 1),
        std::to_string(snapshot.num_propagations),
        std::to_string(snapshot.num_clauses),
        format_fixed(snapshot.average_level, 1),
        format_fixed(snapshot.average_rank, 1),
        std::to_string(snapshot.remaining_vars),

        std::to_string(snapshot.decisions),
        std::to_string(snapshot.trail_level),
        std::to_string(snapshot.trail_rank),
        std::to_string(snapshot.num_restarts),
        std::to_string(snapshot.num_reused_levels),

        std::to_string(snapshot.num_deleted_clauses),
        std::to_string(snapshot.num_demoted_clauses),
        std::to_string(snapshot.num_promoted_clauses),

        std::to_string(snapshot.num_attempt_props),
        std::to_string(snapshot.num_watch_updates),
    };
}

struct GroupName {
    const char* spelling;
    unsigned mask;
};

constexpr GroupName group_names[]{
    {"default", progress_group::defaults},
    {"search", progress_group::search},
    {"deletion", progress_group::deletion},
    {"propagation", progress_group::propagation},
    {"all", progress_group::all},
};

} // namespace

std::optional<unsigned>
parse_progress_groups(const std::string_view text) {
    // The default columns are what makes a line readable as progress, so they
    // are always on and a list only ever adds to them.
    unsigned mask = progress_group::defaults;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t comma = text.find(',', start);
        const std::size_t end = comma == std::string_view::npos ? text.size() : comma;
        const std::string_view name = text.substr(start, end - start);
        if (name.empty()) {
            return std::nullopt;
        }
        bool matched = false;
        for (const GroupName& group : group_names) {
            if (name == group.spelling) {
                mask |= group.mask;
                matched = true;
                break;
            }
        }
        if (!matched) {
            return std::nullopt;
        }
        if (comma == std::string_view::npos) {
            break;
        }
        start = comma + 1;
    }
    return mask;
}

std::string
progress_group_names() {
    std::string names;
    for (const GroupName& group : group_names) {
        if (!names.empty()) {
            names += ", ";
        }
        names += group.spelling;
    }
    return names;
}

ConsoleProgressReporter::ConsoleProgressReporter(std::ostream& out, const unsigned groups)
    : out_(out), groups_(groups) {}

void
ConsoleProgressReporter::write_header(std::ostream& output) {
    output << "c";
    for (const Column& column : columns) {
        if ((column.group & groups_) != 0) {
            output << std::setw(column.width) << column.name;
        }
    }
    output << "\n";
    lines_since_header_ = 0;
    header_written_ = true;
}

void
ConsoleProgressReporter::report(const ProgressSnapshot& snapshot) {
    std::ostringstream output;

    if (!header_written_ || lines_since_header_ >= header_interval) {
        write_header(output);
    }

    const std::array cells = cells_of(snapshot);
    static_assert(std::tuple_size_v<decltype(cells)> == std::size(columns));

    output << "c";
    for (std::size_t index = 0; index < cells.size(); ++index) {
        if ((columns[index].group & groups_) != 0) {
            output << std::setw(columns[index].width) << cells[index];
        }
    }
    output << "\n";
    ++lines_since_header_;

    out_ << output.str() << std::flush;
}

} // namespace xorcle
