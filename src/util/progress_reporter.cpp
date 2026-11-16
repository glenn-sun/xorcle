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
    int column_chars;
    unsigned group;
};

/// @brief The columns, in print order; the header and every line share them.
constexpr Column columns[]{
    {"time", 7, progress_group::defaults},
    {"confl", 11, progress_group::defaults},
    {"confl/s", 9, progress_group::defaults},
    {"props", 9, progress_group::defaults},
    {"clauses", 9, progress_group::defaults},
    {"avg lvl", 11, progress_group::defaults},
    {"avg trl", 11, progress_group::defaults},
    {"var rem", 11, progress_group::defaults},

    {"decs", 12, progress_group::search},
    {"restart", 10, progress_group::search},

    {"delete", 10, progress_group::deletion},

    {"attmpts", 13, progress_group::propagation},
    {"wtch up", 16, progress_group::propagation},
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
        format_fixed(snapshot.average_decision_level, 1),
        format_fixed(snapshot.average_trail_length, 1),
        std::to_string(snapshot.remaining_vars),

        std::to_string(snapshot.decisions),
        std::to_string(snapshot.num_restarts),

        std::to_string(snapshot.num_deleted_clauses),

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
            output << std::setw(column.column_chars) << column.name;
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
            output << std::setw(columns[index].column_chars) << cells[index];
        }
    }
    output << "\n";
    ++lines_since_header_;

    out_ << output.str() << std::flush;
}

} // namespace xorcle
