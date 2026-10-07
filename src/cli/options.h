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
#include <utility>
#include <vector>

#include "algebra/equation_repr.h"
#include "core/solver.h"
#include "heuristics/decisions/csids.h"
#include "heuristics/decisions/decision_procedure.h"
#include "heuristics/decisions/phase.h"
#include "heuristics/deletion/deletion.h"
#include "heuristics/restarts/restart_policy.h"
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
#define XORCLE_OPTION(name, kind, default_value, lo, hi, help)                                     \
    OptionValue<OptionKind::kind>::type name = default_value;
#define XORCLE_ENUM_OPTION(name, type, default_value, help) type name = default_value;
#include "cli/options.inc"

    std::string input_path;
};

/// @brief Which options the command line actually gave, one flag per table row.
struct OptionSeen {
#define XORCLE_OPTION(name, kind, default_value, lo, hi, help) bool name = false;
#define XORCLE_ENUM_OPTION(name, type, default_value, help) bool name = false;
#include "cli/options.inc"
};

// --- Enum values and requirements, as parallel tables ----------------------

/// @brief One accepted spelling of one enum-valued option.
///
/// The enumerator travels as its underlying value, cast back at the one place
/// that knows the type.
struct EnumOptionValue {
    std::string_view option;   ///< The option's table name, e.g. `"decisions"`.
    std::string_view spelling; ///< The accepted value, e.g. `"csids"`.
    int value;
};

/// @brief Every accepted spelling of every enum-valued option, in table order.
inline constexpr EnumOptionValue enum_option_values[]{
#define XORCLE_ENUM_VALUE(option, spelling, enumerator)                                            \
    {#option, spelling, static_cast<int>(enumerator)},
#include "cli/options.inc"
};

/// @brief One `XORCLE_REQUIRES` row, for the help text; `validate_options`
/// reads the same table for the condition itself.
struct OptionRequirement {
    std::string_view option;
    std::string_view needs;
};

inline constexpr OptionRequirement option_requirements[]{
#define XORCLE_REQUIRES(option, condition, needs) {#option, needs},
#include "cli/options.inc"
};

/// @brief What @p option needs to be read at all, or empty when it always is.
inline std::string requirement_text(const std::string_view option) {
    std::string text;
    for (const OptionRequirement& row : option_requirements) {
        if (row.option != option) {
            continue;
        }
        text += text.empty() ? "requires " : " and ";
        text += row.needs;
    }
    return text;
}

// --- Spelling --------------------------------------------------------------

/// @brief The command-line spelling of a table name: `deletion_lbd_window` becomes
/// `--deletion-lbd-window`.
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
/// `--deletion-lbd-window` and `--deletion_lbd_window` are the same option. The dashed
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

/// @brief The requirement clause of an out-of-range message.
///
/// An upper bound at the type's maximum is no constraint, so it is reported as a
/// minimum rather than as a range ending in `18446744073709551615`.
template <class T>
std::string bounds_requirement(const T lo, const T hi) {
    if (hi == std::numeric_limits<T>::max()) {
        return "at least " + std::to_string(lo);
    }
    return "in " + std::to_string(lo) + ".." + std::to_string(hi);
}

inline std::string bounds_requirement(const double lo, const double hi) {
    if (std::isinf(hi)) {
        return "at least " + format_real(lo);
    }
    return "in " + format_real(lo) + ".." + format_real(hi);
}

inline std::uint64_t parse_option_value(OptionKind::U64, std::string_view option,
                                        const std::string& text, std::uint64_t lo,
                                        std::uint64_t hi) {
    const std::uint64_t value = parse_unsigned(option, text);
    if (value < lo || value > hi) {
        throw CliError(std::string(option) + " must be " + bounds_requirement(lo, hi) + ", got '" +
                       text + "'");
    }
    return value;
}

inline std::size_t parse_option_value(OptionKind::Size, std::string_view option,
                                      const std::string& text, std::size_t lo, std::size_t hi) {
    const std::uint64_t value = parse_unsigned(option, text);
    if (value < lo || value > hi) {
        throw CliError(std::string(option) + " must be " + bounds_requirement(lo, hi) + ", got '" +
                       text + "'");
    }
    return static_cast<std::size_t>(value);
}

inline double parse_option_value(OptionKind::Real, std::string_view option, const std::string& text,
                                 double lo, double hi) {
    const double value = parse_real(option, text);
    if (value < lo || value > hi) {
        throw CliError(std::string(option) + " must be " + bounds_requirement(lo, hi) + ", got '" +
                       text + "'");
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

/// @brief How the help text shows a default value.
inline std::string format_option_default(OptionKind::Bool, bool value) {
    return value ? "true" : "false";
}
inline std::string format_option_default(OptionKind::U64, std::uint64_t value) {
    return std::to_string(value);
}
inline std::string format_option_default(OptionKind::Size, std::size_t value) {
    return std::to_string(value);
}
inline std::string format_option_default(OptionKind::Real, double value) {
    return format_real(value);
}
inline std::string format_option_default(OptionKind::Text, std::string_view value) {
    return value.empty() ? "unset" : std::string(value);
}
inline std::string format_option_default(OptionKind::Name, std::string_view value) {
    return std::string(value);
}

/// @brief How the help text states an option's bounds.
inline std::string format_option_bounds(OptionKind::Bool, bool, bool) { return ""; }
inline std::string format_option_bounds(OptionKind::U64, std::uint64_t lo, std::uint64_t hi) {
    if (hi == std::numeric_limits<std::uint64_t>::max()) {
        return "minimum " + std::to_string(lo);
    }
    return "range " + std::to_string(lo) + ".." + std::to_string(hi);
}
inline std::string format_option_bounds(OptionKind::Size, std::size_t lo, std::size_t hi) {
    if (hi == std::numeric_limits<std::size_t>::max()) {
        return "minimum " + std::to_string(lo);
    }
    return "range " + std::to_string(lo) + ".." + std::to_string(hi);
}
inline std::string format_option_bounds(OptionKind::Real, double lo, double hi) {
    if (std::isinf(hi)) {
        return "minimum " + format_real(lo);
    }
    return "range " + format_real(lo) + ".." + format_real(hi);
}
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

#define XORCLE_OPTION(name, kind, default_value, lo, hi, help)                                     \
    append_option_help(out, #name, option_syntax(OptionKind::kind{}), help,                        \
                       format_option_default(OptionKind::kind{}, default_value),                   \
                       format_option_bounds(OptionKind::kind{}, lo, hi));
#define XORCLE_ENUM_OPTION(name, type, default_value, help)                                        \
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

#define XORCLE_OPTION(option_name, kind, default_value, lo, hi, help)                              \
    if (name == #option_name) {                                                                    \
        options.option_name = parse_option_value(OptionKind::kind{}, spelling, value, lo, hi);     \
        seen.option_name = true;                                                                   \
        return;                                                                                    \
    }
#define XORCLE_ENUM_OPTION(option_name, type, default_value, help)                                 \
    if (name == #option_name) {                                                                    \
        options.option_name = parse_enum_option_value<type>(#option_name, value);                  \
        seen.option_name = true;                                                                   \
        return;                                                                                    \
    }
#include "cli/options.inc"

    throw CliError("unknown option '" + spelling + "'");
}

// --- The effective configuration, as the statistics record it --------------

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

#define XORCLE_OPTION(name, kind, default_value, lo, hi, help)                                     \
    settings.push_back(entry(#name, format_option_value(OptionKind::kind{}, options.name),         \
                             format_option_value(OptionKind::kind{}, default_value)));
#define XORCLE_ENUM_OPTION(name, type, default_value, help)                                        \
    settings.push_back(entry(#name, enum_option_label(#name, static_cast<int>(options.name)),      \
                             enum_option_label(#name, static_cast<int>(default_value))));
#include "cli/options.inc"

    return settings;
}

/// @brief Check the constraints that span more than one option.
/// @throws CliError if options conflict with each other.
inline void validate_options(const Options& options, const OptionSeen& seen) {
    // One check per `XORCLE_REQUIRES` row: an option given where nothing would
    // read it is refused, so a run never reports a setting it did not act on.
#define XORCLE_REQUIRES(option, condition, needs)                                                  \
    if (seen.option && !(condition)) {                                                             \
        throw CliError(cli_spelling(#option) + " requires " + needs +                              \
                       "; nothing would read it here");                                            \
    }
#include "cli/options.inc"

    // Trail reuse is a switch rather than a parameter, so it is checked against
    // its value, however it came to be `true`.
    if (options.restart_reuse_trail && options.restart == RestartMethod::none) {
        throw CliError("--restart-reuse-trail requires a restart schedule; pass --restart=luby "
                       "or --restart=lbd");
    }
    // Only CSIDS ranks the levels a restart could keep.
    if (options.restart != RestartMethod::none && options.restart_reuse_trail &&
        options.decisions != DecisionProcedureName::csids) {
        throw CliError("--restart-reuse-trail requires --decisions=csids, the only procedure that "
                       "ranks the levels a restart could keep; pass --restart-reuse-trail=false "
                       "to restart to the root instead");
    }
    if (!parse_progress_groups(options.report).has_value()) {
        throw CliError("--report='" + options.report +
                       "' is not a comma-joined list of: " + progress_group_names());
    }
    if (options.deletion_cadence_coefficient == 0.0) {
        throw CliError("--deletion-cadence-coefficient must be positive");
    }
    if (options.deletion && options.deletion_key == DeletionKey::activity &&
        options.decisions != DecisionProcedureName::csids) {
        throw CliError("--deletion-key=activity requires --decisions=csids");
    }
    // The exact watchlist's cells are packed words.
    if (options.selection == ClauseSelectorName::exact_watchlist &&
        options.equation_repr != EquationRepr::dense) {
        throw CliError("--selection=exact_watchlist requires --equation-repr=dense");
    }
    // A switch rather than a parameter, so it is checked against its value,
    // which the default has already resolved to false under --compact=false.
    if (options.implication_graph && !options.compact) {
        throw CliError("--implication-graph requires --compact=true: without a compaction after "
                       "each feed, which basis of the fed units the rule happened to pick would "
                       "go on steering the search");
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

    // The rule needs a compaction after each feed, so without compaction the
    // implication graph resolves to off unless it was given.
    if (!seen.implication_graph && !options.compact) {
        options.implication_graph = false;
    }

    validate_options(options, seen);
    return options;
}

// --- Projections onto the configuration structs the solver takes -----------

/// @brief The CSIDS parameters, by member name.
inline CsidsConfig csids_config(const Options& options) {
    return CsidsConfig{.initial_activity = options.csids_initial,
                       .decay = options.csids_decay,
                       .bump = options.csids_bump,
                       .intersection_depth = options.csids_intersection_depth};
}

inline CompactionConfig compaction_config(const Options& options) {
    return CompactionConfig{.enabled = options.compact};
}

inline ImplicationGraphConfig implication_graph_config(const Options& options) {
    return ImplicationGraphConfig{.enabled = options.implication_graph,
                                  .clause_cadence = options.implication_graph_clause_cadence,
                                  .cadence = options.implication_graph_cadence,
                                  .max_pairs = options.implication_graph_max_pairs,
                                  .max_descendants = options.implication_graph_max_descendants};
}

inline DeletionConfig deletion_config(const Options& options) {
    return DeletionConfig{.enabled = options.deletion,
                          .cadence_coefficient = options.deletion_cadence_coefficient,
                          .cadence_offset = options.deletion_cadence_offset,
                          .wait = options.deletion_wait,
                          .lbd_window = options.deletion_lbd_window,
                          .eviction_rate = options.deletion_eviction_rate,
                          .key = options.deletion_key};
}

/// @brief Only the cyclic selector runs an attempt budget; under any other,
/// propagation runs to a fixed point.
inline PropagationConfig propagation_config(const Options& options) {
    return PropagationConfig{.miss_rate = options.selection == ClauseSelectorName::cyclic
                                              ? options.propagate_miss_rate
                                              : 0.0,
                             .window = options.propagate_window};
}

inline RestartConfig restart_config(const Options& options) {
    return RestartConfig{.method = options.restart,
                         .luby_unit_conflicts = options.restart_luby_unit,
                         .lbd_fast_window = options.restart_lbd_fast,
                         .lbd_slow_window = options.restart_lbd_slow,
                         .lbd_min_ratio = options.restart_lbd_min_ratio,
                         .lbd_min_conflicts = options.restart_lbd_min_conflicts,
                         .lbd_trail_ratio = options.restart_lbd_trail_ratio,
                         .reuse_trail = options.restart_reuse_trail};
}

// --- Building the solver -----------------------------------------------------

/// @brief The solver `xorcle` runs for @p options over @p num_vars variables,
/// with no clauses yet.
/// @param proof_writer Not owned; null for no proof.
/// @param progress_reporter Not owned; null reports nothing.
/// @param selector Replaces the selector @p options names, when non-null.
inline std::unique_ptr<Solver> make_solver(const Options& options, std::size_t num_vars,
                                           RawProofWriter* proof_writer = nullptr,
                                           ProgressReporter* progress_reporter = nullptr,
                                           std::unique_ptr<ClauseSelector> selector = nullptr) {
    if (selector == nullptr) {
        selector = make_clause_selector(options.selection, num_vars);
    }
    return std::make_unique<Solver>(
        options.equation_repr, num_vars, std::move(selector),
        make_decision_procedure(options.decisions, options.equation_repr, num_vars, options.seed,
                                options.phase, csids_config(options)),
        make_restart_policy(restart_config(options)), proof_writer, compaction_config(options),
        ProgressConfig{.reporter = progress_reporter, .cadence = options.report_cadence},
        options.minimize, deletion_config(options), options.bump_responsible_clauses, options.subsume,
        propagation_config(options), implication_graph_config(options));
}

} // namespace xorcle
