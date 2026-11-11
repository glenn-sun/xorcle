#pragma once

/// @file options.h
/// @brief Everything the command line configures, generated from
/// `cli/options.inc`.
///
/// One X-macro table generates the options struct, the `--name=value` parser,
/// the bounds checks and `--help`. Enum-valued choices use a parallel
/// name-to-enumerator table, so they validate and self-document the same way.

#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <memory>
#include <optional>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "algebra/equation_repr.h"
#include "core/solver.h"
#include "heuristics/decisions/csids.h"
#include "heuristics/decisions/phase.h"
#include "heuristics/decisions/saved_trails.h"
#include "heuristics/deletion/deletion.h"
#include "heuristics/restarts/restart_policy.h"
#include "heuristics/restarts/stabilize.h"
#include "heuristics/selection/clause_selector.h"
#include "util/progress_reporter.h"
#include "util/random.h"
#include "util/stats.h"

namespace xorcle {

/// @brief A malformed command line, or one whose values do not make sense.
class CliError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// --- Option value kinds ----------------------------------------------------

/// @brief Tag types naming the value kinds the option table's `kind` column can
/// hold.
struct OptionKind {
    struct Bool {};  ///< `true`/`false`, also accepting `on`/`off`, `1`/`0`, `yes`/`no`.
    struct U64 {};   ///< Unsigned 64-bit integer; bounds inclusive.
    struct Size {};  ///< `std::size_t`; bounds inclusive.
    struct Real {};  ///< Finite double; bounds inclusive.
    struct Text {};  ///< Free-form string, e.g. a path. Unbounded.
    struct Name {};  ///< A string from a fixed vocabulary the option itself checks. Unbounded.
};

/// @brief The value type an `OptionKind` tag stands for.
template <class Kind>
struct OptionValue;

template <>
struct OptionValue<OptionKind::Bool> {
    using type = bool;
};
template <>
struct OptionValue<OptionKind::U64> {
    using type = std::uint64_t;
};
template <>
struct OptionValue<OptionKind::Size> {
    using type = std::size_t;
};
template <>
struct OptionValue<OptionKind::Real> {
    using type = double;
};
template <>
struct OptionValue<OptionKind::Text> {
    using type = std::string;
};
template <>
struct OptionValue<OptionKind::Name> {
    using type = std::string;
};

// --- The generated structs -------------------------------------------------

/// @brief One member per table row, plus the positional input path.
struct Options {
#define XORCLE_OPTION(group, name, kind, default_value, lo, hi, help)                              \
    OptionValue<OptionKind::kind>::type name = default_value;
#define XORCLE_ENUM_OPTION(group, name, type, default_value, help) type name = default_value;
#include "cli/options.inc"

    std::string input_path;
};

/// @brief Which options the command line actually gave, one flag per table row.
struct OptionSeen {
#define XORCLE_OPTION(group, name, kind, default_value, lo, hi, help) bool name = false;
#define XORCLE_ENUM_OPTION(group, name, type, default_value, help) bool name = false;
#include "cli/options.inc"
};

// --- Enum values and option groups, as parallel tables ---------------------

/// @brief One accepted spelling of one enum-valued option.
///
/// The enumerator travels as its underlying value, cast back at the one place
/// that knows the type.
struct EnumOptionValue {
    std::string_view option;   ///< The option's table name, e.g. `"phase"`.
    std::string_view spelling; ///< The accepted value, e.g. `"rotate"`.
    int value;
};

/// @brief Every accepted spelling of every enum-valued option, in table order.
inline constexpr EnumOptionValue enum_option_values[]{
#define XORCLE_ENUM_VALUE(option, spelling, enumerator)                                            \
    {#option, spelling, static_cast<int>(enumerator)},
#include "cli/options.inc"
};

/// @brief The features whose options are read only when the feature is, one per
/// `XORCLE_GROUP` row.
enum class OptionGroup {
#define XORCLE_GROUP(name, condition, needs) name,
#include "cli/options.inc"
};

/// @brief Whether the options of @p group are read under @p options.
inline bool option_group_is_read(const OptionGroup group, const Options& options) {
    switch (group) {
#define XORCLE_GROUP(name, condition, needs)                                                       \
    case OptionGroup::name:                                                                        \
        return (condition);
#include "cli/options.inc"
    }
    std::unreachable();
}

/// @brief What @p group needs to be read, completing "`--option` requires ...".
inline std::string_view option_group_needs(const OptionGroup group) {
    switch (group) {
#define XORCLE_GROUP(name, condition, needs)                                                       \
    case OptionGroup::name:                                                                        \
        return needs;
#include "cli/options.inc"
    }
    std::unreachable();
}

/// @pre @p option names a table row.
inline OptionGroup option_group_of(const std::string_view option) {
#define XORCLE_OPTION(group, name, kind, default_value, lo, hi, help)                              \
    if (option == #name) {                                                                         \
        return OptionGroup::group;                                                                 \
    }
#define XORCLE_ENUM_OPTION(group, name, type, default_value, help)                                 \
    if (option == #name) {                                                                         \
        return OptionGroup::group;                                                                 \
    }
#include "cli/options.inc"
    assert(false && "no row of the option table has this name");
    std::unreachable();
}

/// @brief What @p option needs to be read at all, or empty when it always is.
inline std::string requirement_text(const std::string_view option) {
    const std::string_view needs = option_group_needs(option_group_of(option));
    return needs.empty() ? std::string() : "requires " + std::string(needs);
}

// --- Spelling --------------------------------------------------------------

/// @brief The command-line spelling of a table name: `deletion_eviction_rate` becomes
/// `--deletion-eviction-rate`.
inline std::string cli_spelling(std::string_view table_name) {
    std::string out = "--";
    for (const char c : table_name) {
        out.push_back(c == '_' ? '-' : c);
    }
    return out;
}

/// @brief The table name of a command-line option name, `--` already stripped.
///
/// Dashes and underscores are interchangeable in an option name, so
/// `--deletion-eviction-rate` and `--deletion_eviction_rate` are the same option. The dashed
/// form is the one `--help` and the error messages use.
inline std::string table_name_of(std::string_view spelling) {
    std::string out;
    for (const char c : spelling) {
        out.push_back(c == '-' ? '_' : c);
    }
    return out;
}

/// @brief Every accepted spelling of one enum option, as `a|b|c`.
inline std::string enum_option_spellings(std::string_view option) {
    std::string out;
    for (const EnumOptionValue& row : enum_option_values) {
        if (row.option != option) {
            continue;
        }
        if (!out.empty()) {
            out += "|";
        }
        out += row.spelling;
    }
    return out;
}

/// @brief The label for an enum option's value: the first spelling naming it.
inline std::string enum_option_label(std::string_view option, int value) {
    for (const EnumOptionValue& row : enum_option_values) {
        if (row.option == option && row.value == value) {
            return std::string(row.spelling);
        }
    }
    assert(false && "no spelling in the option table for this enum value");
    std::unreachable();
}

// --- Value parsing and bounds checking, one overload per kind --------------

inline std::string format_real(const double value) {
    std::ostringstream text;
    text << value;
    return text.str();
}

/// @brief Parse an unsigned integer, rejecting signs and trailing junk.
inline std::uint64_t parse_unsigned(std::string_view option, const std::string& text) {
    if (text.empty() || text[0] == '-' || text[0] == '+') {
        throw CliError(std::string(option) + " expects a non-negative integer, got '" + text + "'");
    }
    try {
        std::size_t consumed = 0;
        const unsigned long long value = std::stoull(text, &consumed);
        if (consumed != text.size()) {
            throw CliError(std::string(option) + " expects an integer, got '" + text + "'");
        }
        return static_cast<std::uint64_t>(value);
    } catch (const std::logic_error&) {
        throw CliError(std::string(option) + " expects an integer, got '" + text + "'");
    }
}

/// @brief Parse a finite double, rejecting trailing junk and infinities.
inline double parse_real(std::string_view option, const std::string& text) {
    try {
        std::size_t consumed = 0;
        const double value = std::stod(text, &consumed);
        if (consumed != text.size() || !std::isfinite(value)) {
            throw CliError(std::string(option) + " expects a finite number, got '" + text + "'");
        }
        return value;
    } catch (const std::logic_error&) {
        throw CliError(std::string(option) + " expects a finite number, got '" + text + "'");
    }
}

inline bool parse_option_value(OptionKind::Bool, std::string_view option, const std::string& text,
                               bool, bool) {
    if (text == "true" || text == "on" || text == "1" || text == "yes") {
        return true;
    }
    if (text == "false" || text == "off" || text == "0" || text == "no") {
        return false;
    }
    throw CliError(std::string(option) + " expects true or false, got '" + text + "'");
}

/// @brief Bounds as `<minimum> lo`, or as `<range> lo..hi` when `hi` constrains.
///
/// An upper bound at the type's maximum, or infinite, is no constraint, so it is
/// reported as a minimum rather than as a range ending in `18446744073709551615`.
template <class T>
std::string format_bounds(const T lo, const T hi, std::string_view minimum,
                          std::string_view range) {
    const auto text = [](const T value) {
        if constexpr (std::is_floating_point_v<T>) {
            return format_real(value);
        } else {
            return std::to_string(value);
        }
    };
    bool unbounded = false;
    if constexpr (std::is_floating_point_v<T>) {
        unbounded = std::isinf(hi);
    } else {
        unbounded = hi == std::numeric_limits<T>::max();
    }
    if (unbounded) {
        return std::string(minimum) + " " + text(lo);
    }
    return std::string(range) + " " + text(lo) + ".." + text(hi);
}

inline std::uint64_t parse_option_value(OptionKind::U64, std::string_view option,
                                        const std::string& text, std::uint64_t lo,
                                        std::uint64_t hi) {
    const std::uint64_t value = parse_unsigned(option, text);
    if (value < lo || value > hi) {
        throw CliError(std::string(option) + " must be " +
                       format_bounds(lo, hi, "at least", "in") + ", got '" + text + "'");
    }
    return value;
}

inline std::size_t parse_option_value(OptionKind::Size, std::string_view option,
                                      const std::string& text, std::size_t lo, std::size_t hi) {
    const std::uint64_t value = parse_unsigned(option, text);
    if (value < lo || value > hi) {
        throw CliError(std::string(option) + " must be " +
                       format_bounds(lo, hi, "at least", "in") + ", got '" + text + "'");
    }
    return static_cast<std::size_t>(value);
}

inline double parse_option_value(OptionKind::Real, std::string_view option, const std::string& text,
                                 double lo, double hi) {
    const double value = parse_real(option, text);
    if (value < lo || value > hi) {
        throw CliError(std::string(option) + " must be " +
                       format_bounds(lo, hi, "at least", "in") + ", got '" + text + "'");
    }
    return value;
}

inline std::string parse_option_value(OptionKind::Text, std::string_view, const std::string& text,
                                      std::string_view, std::string_view) {
    return text;
}

/// @note The vocabulary is not in the table, so `validate_options` checks it.
inline std::string parse_option_value(OptionKind::Name, std::string_view, const std::string& text,
                                      std::string_view, std::string_view) {
    return text;
}

/// @brief Parse one enum option's value, or list every accepted spelling.
template <class Enum>
Enum parse_enum_option_value(std::string_view option, const std::string& text) {
    for (const EnumOptionValue& row : enum_option_values) {
        if (row.option == option && row.spelling == text) {
            return static_cast<Enum>(row.value);
        }
    }
    throw CliError(cli_spelling(option) + "='" + text + "' is not one of: " +
                   enum_option_spellings(option));
}

// --- --help, generated from the same table ---------------------------------

/// @brief The placeholder shown after `=` in the help text.
inline std::string option_syntax(OptionKind::Bool) { return "true|false"; }
inline std::string option_syntax(OptionKind::U64) { return "N"; }
inline std::string option_syntax(OptionKind::Size) { return "N"; }
inline std::string option_syntax(OptionKind::Real) { return "X"; }
inline std::string option_syntax(OptionKind::Text) { return "PATH"; }
inline std::string option_syntax(OptionKind::Name) { return "NAME"; }

/// @brief How the statistics record a value: the text `--name=` would take to
/// reproduce it.
///
/// Unlike the help text, an unset path records as empty.
inline std::string format_option_value(OptionKind::Bool, bool value) {
    return value ? "true" : "false";
}
inline std::string format_option_value(OptionKind::U64, std::uint64_t value) {
    return std::to_string(value);
}
inline std::string format_option_value(OptionKind::Size, std::size_t value) {
    return std::to_string(value);
}
inline std::string format_option_value(OptionKind::Real, double value) {
    return format_real(value);
}
inline std::string format_option_value(OptionKind::Text, std::string_view value) {
    return std::string(value);
}
inline std::string format_option_value(OptionKind::Name, std::string_view value) {
    return std::string(value);
}

/// @brief How the help text shows a default value: as the statistics record it, but
/// an unset path as `unset`.
inline std::string format_option_default(OptionKind::Bool kind, bool value) {
    return format_option_value(kind, value);
}
inline std::string format_option_default(OptionKind::U64 kind, std::uint64_t value) {
    return format_option_value(kind, value);
}
inline std::string format_option_default(OptionKind::Size kind, std::size_t value) {
    return format_option_value(kind, value);
}
inline std::string format_option_default(OptionKind::Real kind, double value) {
    return format_option_value(kind, value);
}
inline std::string format_option_default(OptionKind::Name kind, std::string_view value) {
    return format_option_value(kind, value);
}
inline std::string format_option_default(OptionKind::Text, std::string_view value) {
    return value.empty() ? "unset" : std::string(value);
}

/// @brief How the help text states an option's bounds.
inline std::string format_option_bounds(OptionKind::U64, std::uint64_t lo, std::uint64_t hi) {
    return format_bounds(lo, hi, "minimum", "range");
}
inline std::string format_option_bounds(OptionKind::Size, std::size_t lo, std::size_t hi) {
    return format_bounds(lo, hi, "minimum", "range");
}
inline std::string format_option_bounds(OptionKind::Real, double lo, double hi) {
    return format_bounds(lo, hi, "minimum", "range");
}
inline std::string format_option_bounds(OptionKind::Bool, bool, bool) { return ""; }
inline std::string format_option_bounds(OptionKind::Text, std::string_view, std::string_view) {
    return "";
}
inline std::string format_option_bounds(OptionKind::Name, std::string_view, std::string_view) {
    return "";
}

/// @brief Write one option's two help lines.
inline void append_option_help(std::ostream& out, std::string_view table_name,
                               const std::string& syntax, std::string_view help,
                               const std::string& default_text, const std::string& bounds) {
    out << "  " << cli_spelling(table_name) << "=" << syntax << "\n";
    out << "      " << help;
    // `validate_options` enforces the same rows.
    if (const std::string requires_text = requirement_text(table_name); !requires_text.empty()) {
        out << "; " << requires_text;
    }
    out << " [default: " << default_text;
    if (!bounds.empty()) {
        out << "; " << bounds;
    }
    out << "]\n";
}

/// @brief Print the usage line and every option, from the table.
inline void print_help(std::ostream& out, std::string_view program_name, std::string_view version) {
    out << "xorcle " << version << "\n"
        << "Usage: " << program_name << " [options] <instance>\n"
        << "\n"
        << "A SAT solver for XNF formulas, where clauses are disjunctions of\n"
        << "linear equations modulo 2. See arXiv:2605.15002 for details.\n"
        << "\nOptions:\n";

#define XORCLE_OPTION(group, name, kind, default_value, lo, hi, help)                              \
    append_option_help(out, #name, option_syntax(OptionKind::kind{}), help,                        \
                       format_option_default(OptionKind::kind{}, default_value),                   \
                       format_option_bounds(OptionKind::kind{}, lo, hi));
#define XORCLE_ENUM_OPTION(group, name, type, default_value, help)                                 \
    append_option_help(out, #name, enum_option_spellings(#name), help,                             \
                       enum_option_label(#name, static_cast<int>(default_value)), "");
#include "cli/options.inc"

    out << "  --help, -h\n"
        << "      show this message and exit\n"
        << "  --version\n"
        << "      show the version and exit\n";
}

// --- Parsing a command line ------------------------------------------------

/// @brief Apply one `--name=value` option, given `spelling` with its `--` still
/// on and `value` as the text after the `=`.
/// @throws CliError if the name is unknown or the value is out of range.
inline void apply_option(Options& options, OptionSeen& seen, const std::string& spelling,
                         const std::string& value) {
    const std::string name = table_name_of(std::string_view(spelling).substr(2));

#define XORCLE_OPTION(group, option_name, kind, default_value, lo, hi, help)                       \
    if (name == #option_name) {                                                                    \
        options.option_name = parse_option_value(OptionKind::kind{}, spelling, value, lo, hi);     \
        seen.option_name = true;                                                                   \
        return;                                                                                    \
    }
#define XORCLE_ENUM_OPTION(group, option_name, type, default_value, help)                          \
    if (name == #option_name) {                                                                    \
        options.option_name = parse_enum_option_value<type>(#option_name, value);                  \
        seen.option_name = true;                                                                   \
        return;                                                                                    \
    }
#include "cli/options.inc"

    throw CliError("unknown option '" + spelling + "'");
}

// --- The effective configuration, as the statistics record it --------------

/// @brief Every option and its effective value, in table order.
///
/// This is what makes a statistics row a reproduction recipe, so it is taken
/// from the resolved `Options`.
inline std::vector<ConfigEntry> option_settings(const Options& options) {
    std::vector<ConfigEntry> settings;

    // Each row compares its value against the table's own default, so the
    // console block can show what was asked for rather than the whole table.
    const auto entry = [](const char* name, std::string value, const std::string& default_value) {
        const bool is_default = value == default_value;
        return ConfigEntry{name, std::move(value), is_default};
    };

#define XORCLE_OPTION(group, name, kind, default_value, lo, hi, help)                              \
    settings.push_back(entry(#name, format_option_value(OptionKind::kind{}, options.name),         \
                             format_option_value(OptionKind::kind{}, default_value)));
#define XORCLE_ENUM_OPTION(group, name, type, default_value, help)                                 \
    settings.push_back(entry(#name, enum_option_label(#name, static_cast<int>(options.name)),      \
                             enum_option_label(#name, static_cast<int>(default_value))));
#include "cli/options.inc"

    return settings;
}

/// @brief Check the constraints that span more than one option.
/// @throws CliError if options conflict with each other.
inline void validate_options(const Options& options, const OptionSeen& seen) {
    // An option given where nothing would read it is refused, so a run never
    // reports a setting it did not act on.
#define XORCLE_OPTION(group, name, kind, default_value, lo, hi, help)                              \
    if (seen.name && !option_group_is_read(OptionGroup::group, options)) {                         \
        throw CliError(cli_spelling(#name) + " " + requirement_text(#name) +                       \
                       "; nothing would read it here");                                            \
    }
#define XORCLE_ENUM_OPTION(group, name, type, default_value, help)                                 \
    if (seen.name && !option_group_is_read(OptionGroup::group, options)) {                         \
        throw CliError(cli_spelling(#name) + " " + requirement_text(#name) +                       \
                       "; nothing would read it here");                                            \
    }
#include "cli/options.inc"

    if (!parse_progress_groups(options.report).has_value()) {
        throw CliError("--report='" + options.report +
                       "' is not a comma-joined list of: " + progress_group_names());
    }
    if (options.deletion_cadence_coefficient == 0.0) {
        throw CliError("--deletion-cadence-coefficient must be positive");
    }
}

/// @brief Parse a whole command line.
/// @param arguments The arguments after `argv[0]`.
/// @param help_out Where `--help` is written.
/// @param program_name Name to show in the usage line.
/// @param version Version to show in the help header.
/// @return The options, or `std::nullopt` if `--help` was given and printed.
/// @throws CliError on any malformed or inconsistent command line.
/// @note Nothing here touches the filesystem; the caller checks that
/// `input_path` names a readable file.
inline std::optional<Options> parse_options(const std::vector<std::string>& arguments,
                                            std::ostream& help_out,
                                            std::string_view program_name = "xorcle",
                                            std::string_view version = "") {
    Options options;
    OptionSeen seen;
    std::vector<std::string> positionals;

    for (const std::string& argument : arguments) {
        if (argument == "--help" || argument == "-h") {
            print_help(help_out, program_name, version);
            return std::nullopt;
        }
        if (argument == "--version") {
            help_out << "xorcle " << version << "\n";
            return std::nullopt;
        }
        if (argument.starts_with("-") && !argument.starts_with("--")) {
            throw CliError("'" + argument +
                           "' is not an option. See --help for details.");
        }
        if (!argument.starts_with("--")) {
            positionals.push_back(argument);
            continue;
        }
        const std::size_t equals = argument.find('=');
        if (equals == std::string::npos) {
            throw CliError("option '" + argument + "' needs a value, as --name=value");
        }
        const std::string spelling = argument.substr(0, equals);
        const std::string value = argument.substr(equals + 1);
        apply_option(options, seen, spelling, value);
    }

    if (positionals.size() != 1) {
        throw CliError("expected exactly one input file, got " +
                       std::to_string(positionals.size()) + "; usage: " + std::string(program_name) +
                       " [options] <instance>");
    }
    options.input_path = positionals.front();

    validate_options(options, seen);
    return options;
}

// --- Projections onto the configuration structs the solver takes -----------

/// @brief The CSIDS parameters, by member name.
inline CsidsConfig csids_config(const Options& options) {
    return CsidsConfig{.initial_activity = options.csids_initial,
                       .decay = options.csids_decay,
                       .focused_decay = options.focused_decay,
                       .bump = options.csids_bump,
                       .intersection_depth = options.csids_intersection_depth,
                       .span = options.csids_span,
                       .phase = options.phase,
                       .phase_saving = {.source = options.phase_saving,
                                        .target_reset_conflicts =
                                            options.phase_saving_target_reset,
                                        .rotation_interval = options.phase_rotate_interval,
                                        .focused_source =
                                            options.stabilize
                                                ? std::optional(options.focused_phase_saving)
                                                : std::nullopt}};
}

inline ImplicationGraphConfig implication_graph_config(const Options& options) {
    return ImplicationGraphConfig{.cadence = options.implication_graph_cadence,
                                  .min_new_clauses = options.implication_graph_min_new_clauses,
                                  .max_pairs = options.implication_graph_max_pairs,
                                  .max_descendants = options.implication_graph_max_descendants};
}

inline BveConfig bve_config(const Options& options) {
    return BveConfig{.group_cap = options.bve_group_cap,
                     .max_refusals_in_a_row = options.bve_max_refusals_in_a_row,
                     .success_rate = options.bve_success_rate,
                     .core_shrinking = options.bve_core_shrinking,
                     .gate_max_conflicts = options.bve_gate_max_conflicts,
                     .gate_max_table_cost = options.bve_gate_max_table_cost,
                     .gate_max_clause_share = options.bve_gate_max_clause_share,
                     .root_equations = options.bve_root_equations};
}

inline DeletionConfig deletion_config(const Options& options) {
    return DeletionConfig{.cadence_coefficient = options.deletion_cadence_coefficient,
                          .cadence_offset = options.deletion_cadence_offset,
                          .wait = options.deletion_wait,
                          .eviction_rate = options.deletion_eviction_rate,
                          .key = options.deletion_key};
}

inline RestartConfig restart_config(const Options& options) {
    return RestartConfig{.method = options.restart,
                         .luby_unit_conflicts = options.restart_luby_unit,
                         .lbd_fast_window = options.restart_lbd_fast,
                         .lbd_slow_window = options.restart_lbd_slow,
                         .lbd_min_ratio = options.restart_lbd_min_ratio,
                         .lbd_min_conflicts = options.restart_lbd_min_conflicts,
                         .lbd_trail_ratio = options.restart_lbd_trail_ratio};
}

inline StabilizeConfig stabilize_config(const Options& options) {
    return StabilizeConfig{
        .interval_conflicts = options.stabilize_interval,
        .focused_restarts = {.method = RestartMethod::lbd,
                             .luby_unit_conflicts = 0,
                             .lbd_fast_window = options.focused_restart_lbd_fast,
                             .lbd_slow_window = options.focused_restart_lbd_slow,
                             .lbd_min_ratio = options.focused_restart_lbd_min_ratio,
                             .lbd_min_conflicts = options.focused_restart_lbd_min_conflicts,
                             .lbd_trail_ratio = options.focused_restart_lbd_trail_ratio}};
}

/// @brief The solver's configuration under @p options, each feature present
/// exactly when its option group is read.
/// @param progress_reporter Not owned; null reports nothing.
inline SolverConfig solver_config(const Options& options,
                                  ConsoleProgressReporter* progress_reporter = nullptr) {
    SolverConfig config{
        .renumber = options.compact,
        .minimize = options.minimize,
        .bump_responsible_clauses = options.bump_responsible_clauses,
        .otf_subsume = options.otf_subsume,
        .lbd_window = options.lbd_window,
        .attempt_budget = std::nullopt,
        .deletion = std::nullopt,
        .implication_graph = std::nullopt,
        .vivify = std::nullopt,
        .bve = std::nullopt,
        .inproc_subsume_strengthen = std::nullopt,
        .stabilize = std::nullopt,
        .progress = {.reporter = progress_reporter, .cadence = options.report_cadence}};
    if (option_group_is_read(OptionGroup::attempt_budget, options)) {
        config.attempt_budget = PropagationConfig{.miss_rate = options.propagate_miss_rate,
                                                  .window = options.propagate_window};
    }
    if (option_group_is_read(OptionGroup::deletion, options)) {
        config.deletion = deletion_config(options);
    }
    if (option_group_is_read(OptionGroup::implication_graph, options)) {
        config.implication_graph = implication_graph_config(options);
    }
    if (option_group_is_read(OptionGroup::vivify, options)) {
        config.vivify = VivifyConfig{.rounds = options.vivify_rounds,
                                     .cadence = options.vivify_cadence,
                                     .effort = options.vivify_effort};
    }
    if (option_group_is_read(OptionGroup::bve, options)) {
        config.bve = bve_config(options);
    }
    if (option_group_is_read(OptionGroup::inproc_subsume_strengthen, options)) {
        config.inproc_subsume_strengthen =
            SubsumePassConfig{.cadence = options.inproc_subsume_strengthen_cadence,
                              .effort = options.inproc_subsume_strengthen_effort};
    }
    if (option_group_is_read(OptionGroup::stabilize, options)) {
        config.stabilize = stabilize_config(options);
    }
    return config;
}

// --- Building the solver -----------------------------------------------------

/// @brief The solver `xorcle` runs for @p options over the variables
/// @p used_vars of @p num_vars, with no clauses yet.
/// @param proof_writer Not owned; null for no proof.
/// @param progress_reporter Not owned; null reports nothing.
/// @param make_selector Replaces the selector @p options names, when non-null.
/// @pre `used_vars` is strictly ascending and below `num_vars`.
inline std::unique_ptr<Solver> make_solver(const Options& options, std::size_t num_vars,
                                           std::vector<std::size_t> used_vars,
                                           ProofWriter* proof_writer = nullptr,
                                           ConsoleProgressReporter* progress_reporter = nullptr,
                                           ClauseSelectorFactory make_selector = nullptr) {
    const std::size_t num_used_vars = used_vars.size();
    if (make_selector == nullptr) {
        make_selector = [&options, num_used_vars](const ClauseDatabase& database) {
            return make_clause_selector(options.selection, options.equation_repr, num_used_vars,
                                        database);
        };
    }
    return std::make_unique<Solver>(
        options.equation_repr, num_vars, std::move(used_vars), make_selector,
        std::make_unique<Csids>(options.equation_repr, num_used_vars, options.seed,
                                csids_config(options)),
        make_restart_policy(restart_config(options)), proof_writer,
        solver_config(options, progress_reporter));
}

} // namespace xorcle
