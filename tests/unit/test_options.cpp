/// @file test_options.cpp
/// @brief Tests for the X-macro option table and the command line it generates.

#include <doctest/doctest.h>

#include <sstream>
#include <string>
#include <vector>

#include "cli/options.h"

namespace xorcle {

namespace {

/// @brief Parse a command line, discarding the help stream.
Options parse(const std::vector<std::string>& arguments) {
    std::ostringstream help;
    const std::optional<Options> options = parse_options(arguments, help);
    REQUIRE(options.has_value());
    return *options;
}

/// @brief Parse a command line with the input path already appended.
Options parse_with_input(std::vector<std::string> arguments) {
    arguments.push_back("instance.xnf");
    return parse(arguments);
}

/// @brief The message a rejected command line reports.
template <class Error>
std::string rejection_message(std::vector<std::string> arguments) {
    arguments.push_back("instance.xnf");
    std::ostringstream help;
    try {
        parse_options(arguments, help);
    } catch (const Error& error) {
        return error.what();
    }
    return "<accepted>";
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

} // namespace

TEST_CASE("options: the documented defaults") {
    const Options options = parse_with_input({});
    CHECK(options.input_path == "instance.xnf");
    CHECK(options.equation_repr == EquationRepr::dense);
    CHECK(options.seed == default_seed);
    CHECK(options.compact == true);
    CHECK(options.compact_every == 16);
    CHECK(options.decisions == DecisionProcedureName::csids);
    CHECK(options.csids_initial == doctest::Approx(1.0));
    CHECK(options.csids_decay == doctest::Approx(0.98));
    CHECK(options.csids_bump == doctest::Approx(1.0));
    CHECK(options.phase == DecisionPhase::falsify);
    CHECK(options.selection == ClauseSelectorName::cyclic);
    CHECK(options.restart == RestartMethod::none);
    CHECK(options.proof.empty());
    CHECK(options.stats_file.empty());
    CHECK(options.csids_preset.empty());
}

TEST_CASE("options: --name=value sets each kind") {
    const Options options = parse_with_input({"--equation-repr=sparse",
                                              "--seed=42",
                                              "--compact=false",
                                              "--csids-initial=2.5",
                                              "--csids-decay=0.5",
                                              "--csids-bump=0",
                                              "--phase=satisfy",
                                              "--selection=leading_term_watchlist",
                                              "--restart=luby",
                                              "--restart-luby-unit=7",
                                              "--proof=out.lrup",
                                              "--stats-file=stats.csv"});
    CHECK(options.equation_repr == EquationRepr::sparse);
    CHECK(options.seed == 42);
    CHECK(options.compact == false);
    CHECK(options.csids_initial == doctest::Approx(2.5));
    CHECK(options.csids_decay == doctest::Approx(0.5));
    CHECK(options.csids_bump == doctest::Approx(0.0));
    CHECK(options.phase == DecisionPhase::satisfy);
    CHECK(options.selection == ClauseSelectorName::leading_term_watchlist);
    CHECK(options.restart == RestartMethod::luby);
    CHECK(options.restart_luby_unit == 7);
    CHECK(options.proof == "out.lrup");
    CHECK(options.stats_file == "stats.csv");
}

TEST_CASE("options: a table name's underscores are dashes on the command line") {
    CHECK(cli_spelling("compact_every") == "--compact-every");
    CHECK(table_name_of("compact-every") == "compact_every");
    CHECK(parse_with_input({"--compact-every=64"}).compact_every == 64);
    // Dashes and underscores are interchangeable in an option name, as in
    // CaDiCaL; the dashed form is the one `--help` and the errors show.
    CHECK(parse_with_input({"--compact_every=64"}).compact_every == 64);
}

TEST_CASE("options: booleans accept on/off as well as true/false") {
    CHECK(parse_with_input({"--compact=off"}).compact == false);
    CHECK(parse_with_input({"--compact=on"}).compact == true);
    CHECK(parse_with_input({"--compact=0"}).compact == false);
    CHECK(contains(rejection_message<CliError>({"--compact=maybe"}), "expects true or false"));
}

TEST_CASE("options: values outside the table's bounds are rejected") {
    SUBCASE("a size below its minimum") {
        CHECK(contains(rejection_message<CliError>({"--compact-every=0"}), "must be at least 1"));
        CHECK(contains(rejection_message<CliError>({"--restart-initial=0"}), "must be at least 1"));
        CHECK(contains(rejection_message<CliError>({"--restart-luby-unit=0"}), "must be at least 1"));
    }
    SUBCASE("a real below its minimum") {
        CHECK(contains(rejection_message<CliError>({"--csids-initial=-1"}), "must be at least 0"));
        CHECK(contains(rejection_message<CliError>({"--csids-bump=-0.5"}), "must be at least 0"));
        CHECK(contains(rejection_message<CliError>({"--restart-geometric-factor=0.5"}),
                       "must be at least 1"));
    }
    SUBCASE("the decay admits both of its bounds") {
        CHECK(contains(rejection_message<CliError>({"--csids-decay=1.5"}), "must be in 0..1"));
        CHECK(contains(rejection_message<CliError>({"--csids-decay=-0.1"}), "must be in 0..1"));
        // `1` never decays; `0` keeps only the newest activity.
        CHECK(parse_with_input({"--csids-decay=1"}).csids_decay == doctest::Approx(1.0));
        CHECK(parse_with_input({"--csids-decay=0"}).csids_decay == doctest::Approx(0.0));
        CHECK(parse_with_input({"--csids-decay=0.999"}).csids_decay == doctest::Approx(0.999));
    }
    SUBCASE("non-numeric and signed text") {
        CHECK(contains(rejection_message<CliError>({"--seed=-1"}), "non-negative integer"));
        CHECK(contains(rejection_message<CliError>({"--seed=12x"}), "expects an integer"));
        CHECK(contains(rejection_message<CliError>({"--csids-decay=abc"}), "finite number"));
        CHECK(contains(rejection_message<CliError>({"--csids-initial=inf"}), "finite number"));
    }
}

TEST_CASE("options: an unknown enum value lists the accepted spellings") {
    const std::string message = rejection_message<CliError>({"--phase=unsat"});
    CHECK(contains(message, "--phase='unsat' is not one of"));
    CHECK(contains(message, "falsify|satisfy|random"));
    CHECK(contains(rejection_message<CliError>({"--phase=sat"}), "is not one of"));
    CHECK(contains(rejection_message<CliError>({"--selection=exact_watchlist"}), "is not one of"));
}

TEST_CASE("options: malformed command lines are rejected") {
    CHECK(contains(rejection_message<CliError>({"--seed"}), "needs a value"));
    CHECK(contains(rejection_message<CliError>({"--nonsense=1"}), "unknown option '--nonsense'"));
    // Single-dash arguments are named as such rather than being taken for extra
    // positional arguments.
    CHECK(contains(rejection_message<CliError>({"-d", "clause_vsids"}), "every option has the form --name=value"));
    CHECK(contains(rejection_message<CliError>({"-x", "off"}), "every option has the form --name=value"));

    std::ostringstream help;
    CHECK_THROWS_AS(parse_options({}, help), CliError);
    CHECK_THROWS_AS(parse_options({"a.xnf", "b.xnf"}, help), CliError);
}

TEST_CASE("options: the CSIDS presets set the whole triple") {
    // The triple is conventionally written (A0, delta, alpha) while
    // `CsidsConfig` declares it as (A0, alpha, delta), so these check the
    // members by name.
    SUBCASE("berkmin: A0 = 1, delta = 0, alpha = 0.5") {
        const Options options = parse_with_input({"--decisions=berkmin"});
        CHECK(options.decisions == DecisionProcedureName::csids);
        CHECK(options.csids_preset == "berkmin");
        CHECK(options.csids_initial == doctest::Approx(1.0));
        CHECK(options.csids_bump == doctest::Approx(0.0));
        CHECK(options.csids_decay == doctest::Approx(0.5));
        CHECK(csids_config(options).bump == doctest::Approx(0.0));
        CHECK(csids_config(options).decay == doctest::Approx(0.5));
    }
    SUBCASE("cmtf: A0 = 1, delta = 1, alpha = 0.25") {
        const Options options = parse_with_input({"--decisions=cmtf"});
        CHECK(options.csids_preset == "cmtf");
        CHECK(options.csids_initial == doctest::Approx(1.0));
        CHECK(options.csids_bump == doctest::Approx(1.0));
        CHECK(options.csids_decay == doctest::Approx(0.25));
    }
}

TEST_CASE("options: with --decisions repeated, the last preset named wins") {
    // Presets are recorded during the argument loop and applied once afterwards,
    // so an earlier preset leaves nothing of its triple behind.
    SUBCASE("a plain procedure after a preset restores the defaults") {
        const Options options = parse_with_input({"--decisions=berkmin", "--decisions=csids"});
        CHECK(options.decisions == DecisionProcedureName::csids);
        CHECK(options.csids_preset.empty());
        CHECK(options.csids_decay == doctest::Approx(0.98));
        CHECK(options.csids_bump == doctest::Approx(1.0));
    }
    SUBCASE("a preset after a plain procedure takes effect") {
        const Options options = parse_with_input({"--decisions=csids", "--decisions=berkmin"});
        CHECK(options.csids_preset == "berkmin");
        CHECK(options.csids_decay == doctest::Approx(0.5));
        CHECK(options.csids_bump == doctest::Approx(0.0));
    }
    SUBCASE("a preset after another preset replaces the whole triple") {
        const Options options = parse_with_input({"--decisions=berkmin", "--decisions=cmtf"});
        CHECK(options.csids_preset == "cmtf");
        CHECK(options.csids_decay == doctest::Approx(0.25));
        CHECK(options.csids_bump == doctest::Approx(1.0));
    }
    SUBCASE("a manual override is refused or accepted by the preset finally in force") {
        CHECK(contains(rejection_message<CliError>({"--decisions=berkmin", "--csids-decay=0.7"}),
                       "is a preset"));
        const Options options = parse_with_input(
            {"--decisions=berkmin", "--decisions=csids", "--csids-decay=0.7"});
        CHECK(options.csids_preset.empty());
        CHECK(options.csids_decay == doctest::Approx(0.7));
    }
}

TEST_CASE("options: a preset rejects an accompanying manual override") {
    for (const std::string preset : {"berkmin", "cmtf"}) {
        for (const std::string override_option :
             {"--csids-initial=2", "--csids-decay=0.9", "--csids-bump=3"}) {
            const std::string message =
                rejection_message<CliError>({"--decisions=" + preset, override_option});
            CHECK(contains(message, "--decisions=" + preset));
            CHECK(contains(message, "is a preset"));
        }
    }
    // Order does not matter: the check runs once, after the whole line.
    CHECK(contains(rejection_message<CliError>({"--csids-decay=0.9", "--decisions=cmtf"}),
                   "is a preset"));
    // Without a preset the same override is fine.
    CHECK(parse_with_input({"--decisions=csids", "--csids-decay=0.9"}).csids_decay ==
          doctest::Approx(0.9));
}

TEST_CASE("options: --proof with compaction on is refused, naming the fix") {
    const std::string message = rejection_message<LrupError>({"--proof=out.lrup"});
    CHECK(contains(message, "--proof requires --compact=false"));
    // It refuses; it does not flip the compaction default.
    const Options options = parse_with_input({"--proof=out.lrup", "--compact=false"});
    CHECK(options.compact == false);
    CHECK(options.proof == "out.lrup");
}

TEST_CASE("options: enum labels are the first spelling naming an enumerator") {
    CHECK(enum_option_label("decisions", static_cast<int>(DecisionProcedureName::csids)) == "csids");
    CHECK(enum_option_label("phase", static_cast<int>(DecisionPhase::falsify)) == "falsify");
    CHECK(enum_option_label("selection", static_cast<int>(ClauseSelectorName::cyclic)) == "cyclic");
    CHECK(enum_option_spellings("equation_repr") == "dense|sparse");
    CHECK(enum_option_spellings("restart") == "none|geometric|luby");
}

TEST_CASE("options: --help is generated from the table") {
    std::ostringstream help;
    CHECK_FALSE(parse_options({"--help"}, help, "xorcle", "0.0.0-test").has_value());
    const std::string text = help.str();

    CHECK(contains(text, "Usage: xorcle [options] <input>"));
    CHECK(contains(text, "--equation-repr=dense|sparse"));
    CHECK(contains(text, "[default: dense]"));
    CHECK(contains(text, "--seed=N"));
    CHECK(contains(text, "[default: 0; minimum 0]"));
    CHECK(contains(text, "--compact=true|false"));
    CHECK(contains(text, "--compact-every=N"));
    CHECK(contains(text, "[default: 16; minimum 1]"));
    CHECK(contains(text, "--csids-decay=X"));
    CHECK(contains(text, "[default: 0.98; range 0..1]"));
    CHECK(contains(text, "--decisions=csids|variable_vsids|fixed_literals|berkmin|cmtf"));
    CHECK(contains(text, "--phase=falsify|satisfy|random"));
    CHECK(contains(text, "--selection=cyclic|leading_term_watchlist"));
    CHECK(contains(text, "--proof=PATH"));
    CHECK(contains(text, "--stats-file=PATH"));
    CHECK(contains(text, "--help"));

    CHECK(contains(text, "--restart=none|geometric|luby"));
    CHECK(contains(text, "EXPERIMENTAL: restart schedule"));
    CHECK(contains(text, "EXPERIMENTAL: growth factor"));
    CHECK(contains(text, "EXPERIMENTAL: first geometric conflict budget"));
    CHECK(contains(text, "EXPERIMENTAL: conflict budget for a Luby value of 1"));
}

TEST_CASE("options: the configuration projections carry the values across") {
    const Options options = parse_with_input({"--compact=false", "--compact-every=8",
                                              "--restart=geometric",
                                              "--restart-geometric-factor=2",
                                              "--restart-initial=25", "--restart-luby-unit=50"});
    const CompactionConfig compaction = compaction_config(options);
    CHECK(compaction.enabled == false);
    CHECK(compaction.conflict_interval == 8);

    const RestartConfig restart = restart_config(options);
    CHECK(restart.method == RestartMethod::geometric);
    CHECK(restart.geometric_multiplier == doctest::Approx(2.0));
    CHECK(restart.geometric_initial_conflicts == 25);
    CHECK(restart.luby_unit_conflicts == 50);
}

} // namespace xorcle
