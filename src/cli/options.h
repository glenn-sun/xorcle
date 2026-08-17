#pragma once

/// @file options.h
/// @brief Everything the command line configures, generated from
/// `cli/options.inc`.
///
/// One X-macro table generates the options struct, the `--name=value` parser,
/// the bounds checks and `--help`, following CaDiCaL and Kissat. Enum-valued
/// choices use a parallel name-to-enumerator table, so they validate and
/// self-document the same way.
///
/// Header-only, so the parser links into the unit-test binary as well as the
/// executable.
///
/// Everything here reports user error by throwing `CliError`; `main` catches it,
/// prints it, and exits non-zero. Nothing here asserts on input.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <optional>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "algebra/algebra.h"
#include "core/solver.h"
#include "heuristics/decisions/csids.h"
#include "heuristics/decisions/decision_procedure.h"
#include "heuristics/decisions/phase.h"
#include "heuristics/restarts/restart_policy.h"
#include "heuristics/selection/clause_selector.h"
#include "io/lrup_logger.h"
#include "util/random.h"

namespace xorcle {

/// @brief A malformed command line, or one whose values do not make sense.
///
/// User-facing input, so an exception caught in `main` rather than an assert.
class CliError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// --- Option value kinds ----------------------------------------------------

/// @brief Tag types naming the value kinds the option table's `kind` column can
/// hold.
///
/// Tags rather than storage types: `Real` and `Ratio` are both `double` and
/// differ only in whether their bounds are inclusive, so they have to be
/// distinct types for overload resolution to tell them apart.
struct OptionKind {
    struct Bool {};  ///< `true`/`false`, also accepting `on`/`off`, `1`/`0`, `yes`/`no`.
    struct U64 {};   ///< Unsigned 64-bit integer; bounds inclusive.
    struct Size {};  ///< `std::size_t`; bounds inclusive.
    struct Real {};  ///< Finite double; bounds inclusive.
    struct Ratio {}; ///< Finite double; bounds **exclusive**, as @f$0 < \alpha < 1@f$ needs.
    struct Text {};  ///< Free-form string, e.g. a path. Unbounded.
};

/// @brief The storage type an `OptionKind` tag stands for.
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
struct OptionValue<OptionKind::Ratio> {
    using type = double;
};
template <>
struct OptionValue<OptionKind::Text> {
    using type = std::string;
};

// --- The generated structs -------------------------------------------------

/// @brief One member per table row, plus the two things that are not options:
/// the positional input path, and which `--decisions=` preset was chosen.
struct Options {
#define XORCLE_OPTION(name, kind, default_value, lo, hi, help)                                     \
    OptionValue<OptionKind::kind>::type name = default_value;
#define XORCLE_ENUM_OPTION(name, type, default_value, help) type name = default_value;
#include "cli/options.inc"

    std::string input_path;

    /// @brief The `--decisions=` preset in force, or empty.
    ///
    /// Presets fix the whole CSIDS triple, so an accompanying manual override is
    /// rejected rather than silently losing.
    std::string csids_preset;
};

/// @brief Which options the command line actually gave, one flag per table row.
struct OptionSeen {
#define XORCLE_OPTION(name, kind, default_value, lo, hi, help) bool name = false;
#define XORCLE_ENUM_OPTION(name, type, default_value, help) bool name = false;
#include "cli/options.inc"
};

// --- Enum values and presets, as parallel tables ---------------------------

/// @brief One accepted spelling of one enum-valued option.
///
/// Flat rather than one array per option, because a single array cannot hold
/// several enumeration types; the enumerator travels as its underlying value and
/// is cast back at the one place that knows the type.
struct EnumOptionValue {
    std::string_view option;   ///< The option's table name, e.g. `"decisions"`.
    std::string_view spelling; ///< The accepted value, e.g. `"berkmin"`.
    int value;
};

/// @brief Every accepted spelling of every enum-valued option, in table order.
inline constexpr EnumOptionValue enum_option_values[]{
#define XORCLE_ENUM_VALUE(option, spelling, enumerator)                                            \
    {#option, spelling, static_cast<int>(enumerator)},
#include "cli/options.inc"
};

/// @brief A `--decisions=` spelling that fixes the whole CSIDS triple.
struct CsidsPreset {
    std::string_view spelling; ///< The `--decisions=` value, e.g. `"cmtf"`.
    CsidsConfig config;
};

/// @brief The CSIDS presets, from the same table.
inline constexpr CsidsPreset csids_presets[]{
#define XORCLE_PRESET(spelling, config) {#spelling, config},
#include "cli/options.inc"
};

inline const CsidsPreset* find_csids_preset(std::string_view spelling) {
    for (const CsidsPreset& preset : csids_presets) {
        if (preset.spelling == spelling) {
            return &preset;
        }
    }
    return nullptr;
}

// --- Spelling --------------------------------------------------------------

/// @brief The command-line spelling of a table name: `compact_every` becomes
/// `--compact-every`.
inline std::string cli_spelling(std::string_view table_name) {
    std::string out = "--";
    for (const char c : table_name) {
        out.push_back(c == '_' ? '-' : c);
    }
    return out;
}

/// @brief The table name of a command-line option name, `--` already stripped.
///
/// Dashes and underscores are interchangeable in an option name, as in CaDiCaL,
/// so `--compact-every` and `--compact_every` are the same option. The dashed
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
///
/// `--decisions=berkmin` and `--decisions=csids` both select
/// `DecisionProcedureName::csids`, and the table lists `csids` first, so that is
/// what the statistics report.
inline std::string enum_option_label(std::string_view option, int value) {
    for (const EnumOptionValue& row : enum_option_values) {
        if (row.option == option && row.value == value) {
            return std::string(row.spelling);
        }
    }
    return {};
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

/// @brief A finite double strictly inside its bounds.
inline double parse_option_value(OptionKind::Ratio, std::string_view option,
                                 const std::string& text, double lo, double hi) {
    const double value = parse_real(option, text);
    if (value <= lo || value >= hi) {
        throw CliError(std::string(option) + " must satisfy " + format_real(lo) + " < value < " +
                       format_real(hi) + ", got '" + text + "'");
    }
    return value;
}

inline std::string parse_option_value(OptionKind::Text, std::string_view, const std::string& text,
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
inline std::string option_syntax(OptionKind::Ratio) { return "X"; }
inline std::string option_syntax(OptionKind::Text) { return "PATH"; }

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
inline std::string format_option_default(OptionKind::Ratio, double value) {
    return format_real(value);
}
inline std::string format_option_default(OptionKind::Text, std::string_view value) {
    return value.empty() ? "unset" : std::string(value);
}

/// @brief How the help text states an option's bounds.
///
/// A bound at the type's maximum is shown as a minimum instead of printing
/// `18446744073709551615`, which says nothing a reader wants.
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
inline std::string format_option_bounds(OptionKind::Ratio, double lo, double hi) {
    return "range " + format_real(lo) + " < value < " + format_real(hi);
}
inline std::string format_option_bounds(OptionKind::Text, std::string_view, std::string_view) {
    return "";
}

/// @brief Write one option's two help lines.
inline void append_option_help(std::ostream& out, std::string_view table_name,
                               const std::string& syntax, std::string_view help,
                               const std::string& default_text, const std::string& bounds) {
    out << "  " << cli_spelling(table_name) << "=" << syntax << "\n";
    out << "      " << help << " [default: " << default_text;
    if (!bounds.empty()) {
        out << "; " << bounds;
    }
    out << "]\n";
}

/// @brief Print the usage line and every option, from the table.
inline void print_help(std::ostream& out, std::string_view program_name, std::string_view version) {
    out << "xorcle " << version << "\n"
        << "Usage: " << program_name << " [options] <input>\n"
        << "\n"
        << "Solves an XNF instance: a conjunction of clauses, each a disjunction\n"
        << "of linear equations modulo 2.\n"
        << "\nOptions:\n";

#define XORCLE_OPTION(name, kind, default_value, lo, hi, help)                                     \
    append_option_help(out, #name, option_syntax(OptionKind::kind{}), help,                        \
                       format_option_default(OptionKind::kind{}, default_value),                   \
                       format_option_bounds(OptionKind::kind{}, lo, hi));
#define XORCLE_ENUM_OPTION(name, type, default_value, help)                                        \
    append_option_help(out, #name, enum_option_spellings(#name), help,                             \
                       enum_option_label(#name, static_cast<int>(default_value)), "");
#include "cli/options.inc"

    out << "  --help\n"
        << "      show this message and exit\n";

    out << "\nNotes:\n"
        << "  --decisions=berkmin and --decisions=cmtf are presets: each fixes the\n"
        << "  whole CSIDS triple, so combining one with a --csids-* option is an\n"
        << "  error rather than a silent override.\n"
        << "\n"
        << "  --proof requires --compact=false, and does not turn compaction off for\n"
        << "  you: it refuses instead. Compaction changes the search rather than only\n"
        << "  its bookkeeping, so a run that silently disabled it would answer a\n"
        << "  different question than the one asked.\n"
        << "\n"
        << "  The restart options are EXPERIMENTAL and untuned.\n";
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

/// @brief Apply the `--decisions=` preset, if the given spelling is one.
///
/// Called after `apply_option` has already resolved the enumerator, so this only
/// has to overwrite the CSIDS triple and record which preset is in force. The
/// values are copied from `csids_berkmin_config` and `csids_cmtf_config` by
/// member name rather than retyped: `CsidsConfig` declares the triple as
/// @f$(A_0, \alpha, \Delta)@f$ and the literature usually writes it as
/// @f$(A_0, \Delta, \alpha)@f$, so a positional copy would silently swap the
/// bump and the decay.
inline void apply_decisions_preset(Options& options, const std::string& value) {
    const CsidsPreset* preset = find_csids_preset(value);
    if (preset == nullptr) {
        options.csids_preset.clear();
        return;
    }
    options.csids_preset = value;
    options.csids_initial = preset->config.initial_activity;
    options.csids_decay = preset->config.decay;
    options.csids_bump = preset->config.bump;
}

/// @brief Check the constraints that span more than one option.
/// @throws CliError if a preset was combined with a manual CSIDS override.
/// @throws LrupError if `--proof` was given with compaction on.
inline void validate_options(const Options& options, const OptionSeen& seen) {
    if (!options.csids_preset.empty() &&
        (seen.csids_initial || seen.csids_decay || seen.csids_bump)) {
        throw CliError("--decisions=" + options.csids_preset +
                       " is a preset that fixes A0, alpha and delta; it does not accept "
                       "--csids-initial, --csids-decay or --csids-bump");
    }
    // Refuse rather than silently flipping --compact. The message names the fix.
    require_compaction_disabled_for_proof(!options.proof.empty(), options.compact);
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
        // Single-dash arguments are rejected outright: silently counting them as
        // extra positionals would turn a typo into "expected exactly one input".
        if (argument.starts_with("-") && !argument.starts_with("--")) {
            throw CliError("'" + argument +
                           "' is not an option; every option has the form --name=value, "
                           "listed by --help");
        }
        // A bare positional token is the input path and nothing else; there is no
        // sniffing of trailing parameters and no path-shaped heuristic.
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
        if (spelling == "--decisions") {
            apply_decisions_preset(options, value);
        }
    }

    if (positionals.size() != 1) {
        throw CliError("expected exactly one input file, got " +
                       std::to_string(positionals.size()) + "; usage: " + std::string(program_name) +
                       " [options] <input>");
    }
    options.input_path = positionals.front();

    validate_options(options, seen);
    return options;
}

// --- Projections onto the configuration structs the solver takes -----------

/// @brief The CSIDS triple, by member name (see `CsidsConfig`'s warning).
inline CsidsConfig csids_config(const Options& options) {
    return CsidsConfig{.initial_activity = options.csids_initial,
                       .decay = options.csids_decay,
                       .bump = options.csids_bump};
}

inline CompactionConfig compaction_config(const Options& options) {
    return CompactionConfig{.enabled = options.compact, .conflict_interval = options.compact_every};
}

inline RestartConfig restart_config(const Options& options) {
    return RestartConfig{.method = options.restart,
                         .geometric_multiplier = options.restart_geometric_factor,
                         .geometric_initial_conflicts = options.restart_initial,
                         .luby_unit_conflicts = options.restart_luby_unit};
}

} // namespace xorcle
