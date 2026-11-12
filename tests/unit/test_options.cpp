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
    arguments.emplace_back("instance.xnf");
    return parse(arguments);
}

/// @brief The message a rejected command line reports.
template <class Error>
std::string rejection_message(std::vector<std::string> arguments) {
    arguments.emplace_back("instance.xnf");
    std::ostringstream help;
    try {
        parse_options(arguments, help);
    } catch (const Error& error) {
        return error.what();
    }
    return "<accepted>";
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.contains(needle);
}

} // namespace

// The choices, not the tuned numbers: every numeric default is read from the
// table below, and "a recorded value knows whether it is the default" checks
// that an empty command line lands on each of them.
TEST_CASE("options: the documented defaults") {
    const Options options = parse_with_input({});
    CHECK(options.input_path == "instance.xnf");
    CHECK(options.equation_repr == EquationRepr::dense);
    CHECK(options.seed == default_seed);
    CHECK(options.compact == true);
    CHECK(options.phase_default == DecisionPhase::falsify);
    CHECK(options.phase_saving == PhaseSaving::rotate);
    CHECK(options.selection == ClauseSelectorName::exact_watchlist);
    CHECK(options.restart == RestartMethod::none);
    CHECK(options.deletion == true);
    CHECK(options.deletion_key == DeletionKey::activity);
    CHECK(options.csids_bump_learned_positions == false);
    CHECK(options.implication_graph == true);
    CHECK(options.proof.empty());
    CHECK(options.stats_file.empty());
    CHECK(options.report_type == "default");
}

TEST_CASE("options: --name=value sets each kind") {
    const Options options = parse_with_input({"--equation-repr=sparse",
                                              "--selection=cyclic",
                                              "--seed=42",
                                              "--compact=false",
                                              "--csids-initial=2.5",
                                              "--csids-decay=0.5",
                                              "--csids-bump=0",
                                              "--phase-default=satisfy",
                                              "--deletion=false",
                                              "--otf-subsume=false",
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
    CHECK(options.phase_default == DecisionPhase::satisfy);
    CHECK(options.deletion == false);
    CHECK(options.otf_subsume == false);
    CHECK(options.restart == RestartMethod::luby);
    CHECK(options.restart_luby_unit == 7);
    CHECK(options.proof == "out.lrup");
    CHECK(options.stats_file == "stats.csv");
}

TEST_CASE("options: a table name's underscores are dashes on the command line") {
    CHECK(cli_spelling("implication_graph_cadence") == "--implication-graph-cadence");
    CHECK(table_name_of("implication-graph-cadence") == "implication_graph_cadence");
    CHECK(parse_with_input({"--implication-graph-cadence=64"}).implication_graph_cadence == 64);
    // Dashes and underscores are interchangeable in an option name, as in
    // CaDiCaL; the dashed form is the one `--help` and the errors show.
    CHECK(parse_with_input({"--implication_graph_cadence=64"}).implication_graph_cadence == 64);
}

TEST_CASE("options: booleans accept on/off as well as true/false") {
    CHECK(parse_with_input({"--compact=off"}).compact == false);
    CHECK(parse_with_input({"--compact=on"}).compact == true);
    CHECK(parse_with_input({"--compact=0"}).compact == false);
    CHECK(contains(rejection_message<CliError>({"--compact=maybe"}), "expects true or false"));
}

TEST_CASE("options: values outside the table's bounds are rejected") {
    SUBCASE("a size below its minimum") {
        CHECK(contains(rejection_message<CliError>({"--implication-graph-cadence=0"}),
                       "must be at least 1"));
        CHECK(contains(rejection_message<CliError>({"--restart-lbd-min-conflicts=0"}), "must be at least 1"));
    }
    SUBCASE("a real below its minimum") {
        CHECK(contains(rejection_message<CliError>({"--csids-initial=-1"}), "must be at least 0"));
        CHECK(contains(rejection_message<CliError>({"--csids-bump=-0.5"}), "must be at least 0"));
        CHECK(contains(rejection_message<CliError>({"--restart-lbd-min-ratio=0.5"}),
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
    const std::string message = rejection_message<CliError>({"--phase-default=unsat"});
    CHECK(contains(message, "--phase-default='unsat' is not one of"));
    CHECK(contains(message, "falsify|satisfy|random"));
    CHECK(contains(rejection_message<CliError>({"--phase-default=sat"}), "is not one of"));
    CHECK(contains(rejection_message<CliError>({"--selection=exact"}), "is not one of"));
}

TEST_CASE("options: malformed command lines are rejected") {
    CHECK(contains(rejection_message<CliError>({"--seed"}), "needs a value"));
    CHECK(contains(rejection_message<CliError>({"--nonsense=1"}), "unknown option '--nonsense'"));
    // Single-dash arguments are named as such rather than being taken for extra
    // positional arguments.
    CHECK(contains(rejection_message<CliError>({"-d", "clause_vsids"}), "is not an option"));
    CHECK(contains(rejection_message<CliError>({"-x", "off"}), "is not an option"));

    std::ostringstream help;
    CHECK_THROWS_AS(parse_options({}, help), CliError);
    CHECK_THROWS_AS(parse_options({"a.xnf", "b.xnf"}, help), CliError);
}

TEST_CASE("options: --proof goes with compaction on or off") {
    const Options options = parse_with_input({"--proof=out.lrup"});
    CHECK(options.proof == "out.lrup");
    CHECK(parse_with_input({"--proof=out.lrup", "--compact=false"}).proof == "out.lrup");
}

TEST_CASE("options: enum labels are the first spelling naming an enumerator") {
    CHECK(enum_option_label("phase_default", static_cast<int>(DecisionPhase::falsify)) == "falsify");
    CHECK(enum_option_label("selection", static_cast<int>(ClauseSelectorName::cyclic)) == "cyclic");
    CHECK(enum_option_spellings("equation_repr") == "dense|sparse");
    CHECK(enum_option_spellings("restart") == "none|luby|lbd");
}

TEST_CASE("options: --help is generated from the table") {
    std::ostringstream help;
    CHECK_FALSE(parse_options({"--help"}, help, "xorcle", "0.0.0-test").has_value());
    const std::string text = help.str();

    CHECK(contains(text, "Usage: xorcle [options] <instance>"));
    CHECK(contains(text, "--equation-repr=dense|sparse"));
    CHECK(contains(text, "[default: dense]"));
    CHECK(contains(text, "--seed=N"));
    CHECK(contains(text, "[default: 0; minimum 0]"));
    CHECK(contains(text, "--compact=true|false"));
    CHECK(contains(text, "[default: " + std::to_string(Options{}.implication_graph_cadence) +
                             "; minimum 1]"));
    CHECK(contains(text, "--csids-decay=X"));
    CHECK(contains(text, "[default: " + format_real(Options{}.csids_decay) + "; range 0..1]"));
    CHECK(contains(text, "--phase-default=falsify|satisfy|random"));
    CHECK(contains(text, "--phase-saving=both|target-only|recent-only|none|rotate"));
    CHECK(contains(text, "--selection=cyclic|exact-watchlist"));
    CHECK(contains(text, "--proof=PATH"));
    CHECK(contains(text, "--stats-file=PATH"));
    CHECK(contains(text, "--help, -h"));
    CHECK(contains(text, "--version"));

    CHECK(contains(text, "--deletion=true|false"));
    CHECK(contains(text, "periodically delete some learned clauses [default: true]"));
    CHECK(contains(text, "--deletion-cadence-coefficient=X"));
    CHECK(contains(text, "--deletion-cadence-offset=N"));
    CHECK(contains(text, "--deletion-eviction-rate=X"));
    CHECK(contains(text, "--deletion-wait=X"));
    CHECK(contains(text, "--deletion-key=lbd|activity"));

    // The requirement table feeds the help, so a refusal is never a surprise.
    CHECK(contains(text, "deletion pass (must be positive); requires --deletion=true"));
    CHECK(contains(text, "fast/slow ratio needed for LBD restart; requires --restart=lbd"));

    CHECK(contains(text, "--restart=none|luby|lbd"));
    CHECK(contains(text, "restart schedule"));
    CHECK(contains(text, "conflicts for a luby value of 1"));
    CHECK(contains(text, "window of the fast LBD average"));
    // A help of several lines continues under its first.
    CHECK(contains(text, "of its recent\n      average (0 never blocks); requires --restart=lbd"));

    CHECK(contains(text, "--lbd-window=N"));
    CHECK(contains(text, "window of each clause's recent average LBD"));
}

TEST_CASE("options: the luby budget is a positive count") {
    CHECK(contains(rejection_message<CliError>({"--restart=luby", "--restart-luby-unit=0"}),
                   "--restart-luby-unit"));
    CHECK_NOTHROW(parse_with_input({"--restart=luby"}));
    CHECK_NOTHROW(parse_with_input({"--restart=luby", "--restart-luby-unit=4000"}));
}


TEST_CASE("options: the exact watchlist takes deletion, subsumption and sparse rows") {
    const Options options = parse_with_input({"--selection=exact-watchlist"});
    CHECK(options.deletion);
    CHECK(options.otf_subsume);
    CHECK_FALSE(solver_config(options).attempt_budget.has_value());
    CHECK(parse_with_input({"--equation-repr=sparse"}).equation_repr == EquationRepr::sparse);
    CHECK(parse_with_input({"--selection=cyclic", "--equation-repr=sparse"}).equation_repr ==
          EquationRepr::sparse);
}

TEST_CASE("options: deletion's parameters are checked") {
    CHECK(contains(rejection_message<CliError>({"--deletion-cadence-coefficient=0"}),
                   "--deletion-cadence-coefficient must be > 0, got '0'"));
    CHECK(contains(rejection_message<CliError>({"--deletion-eviction-rate=1.5"}),
                   "--deletion-eviction-rate"));
    CHECK_NOTHROW(parse_with_input({"--deletion-eviction-rate=0"}));
    CHECK_NOTHROW(parse_with_input({"--deletion-eviction-rate=1"}));

    CHECK(parse_with_input({"--deletion=false"}).deletion == false);
    CHECK(parse_with_input({"--deletion-key=lbd"}).deletion == true);
    CHECK_NOTHROW(parse_with_input({"--deletion=true", "--deletion-key=activity"}));
    CHECK(parse_with_input({}).deletion == true);
    CHECK(parse_with_input({"--selection=cyclic"}).deletion == true);
}

TEST_CASE("options: the LBD window is read whether or not deletion is on") {
    CHECK(contains(rejection_message<CliError>({"--lbd-window=0"}), "must be at least 1"));
    CHECK(solver_config(parse_with_input({"--lbd-window=1"})).lbd_window == 1);
    CHECK(solver_config(parse_with_input({"--deletion=false", "--lbd-window=3"})).lbd_window == 3);
    CHECK(solver_config(parse_with_input({})).lbd_window == 2);
    CHECK(contains(rejection_message<CliError>({"--deletion-lbd-window=3"}),
                   "unknown option '--deletion-lbd-window'"));
}

TEST_CASE("options: a feature is configured exactly when its group is read") {
    const SolverConfig defaults = solver_config(parse_with_input({}));
    CHECK(defaults.renumber);
    CHECK_FALSE(defaults.attempt_budget.has_value());
    CHECK(defaults.deletion.has_value());
    CHECK(defaults.implication_graph.has_value());
    CHECK(defaults.vivify.has_value());
    CHECK(defaults.bve.has_value());
    CHECK_FALSE(defaults.inproc_subsume_strengthen.has_value());
    CHECK_FALSE(defaults.stabilize.has_value());

    const SolverConfig off = solver_config(parse_with_input(
        {"--deletion=false", "--implication-graph=false", "--vivify=false", "--bve=false",
         "--inproc-subsume-strengthen=true", "--stabilize=true"}));
    CHECK_FALSE(off.deletion.has_value());
    CHECK_FALSE(off.implication_graph.has_value());
    CHECK_FALSE(off.vivify.has_value());
    CHECK_FALSE(off.bve.has_value());
    CHECK(off.inproc_subsume_strengthen.has_value());
    CHECK(off.stabilize.has_value());
}

TEST_CASE("options: the configuration projections carry the values across") {
    // Each option is given where something reads it: the LBD ratio needs the LBD
    // schedule, so the Luby settings are checked in their own parse below.
    const Options options = parse_with_input({"--compact=false",
                                              "--restart=lbd", "--restart-lbd-min-ratio=1.25",
                                              "--deletion=true",
                                              "--deletion-cadence-coefficient=0.75",
                                              "--deletion-cadence-offset=250",
                                              "--deletion-eviction-rate=0.25",
                                              "--deletion-wait=7.5",
                                              "--deletion-key=activity"});
    const DeletionConfig deletion = deletion_config(options);
    CHECK(deletion.cadence_coefficient == doctest::Approx(0.75));
    CHECK(deletion.cadence_offset == 250);
    CHECK(deletion.eviction_rate == doctest::Approx(0.25));
    CHECK(deletion.wait == doctest::Approx(7.5));
    CHECK(deletion.key == DeletionKey::activity);

    CHECK(solver_config(options).renumber == false);

    const RestartConfig restart = restart_config(options);
    CHECK(restart.method == RestartMethod::lbd);
    CHECK(restart.lbd_min_ratio == doctest::Approx(1.25));

    const RestartConfig luby =
        restart_config(parse_with_input({"--restart=luby", "--restart-luby-unit=50"}));
    CHECK(luby.method == RestartMethod::luby);
    CHECK(luby.luby_unit_conflicts == 50);

    CHECK(solver_config(parse_with_input({})).renumber == true);
}

TEST_CASE("options: --stabilize's focused mode takes its own restarts and phase saving") {
    const auto refusal = [](std::vector<std::string> arguments) {
        return rejection_message<CliError>(std::move(arguments));
    };
    CHECK(refusal({"--stabilize-interval=50"})
              .starts_with("--stabilize-interval requires --stabilize=true"));
    CHECK(refusal({"--focused-decay=0.6"}).starts_with("--focused-decay requires --stabilize=true"));
    CHECK(refusal({"--focused-restart-lbd-slow=1000"})
              .starts_with("--focused-restart-lbd-slow requires --stabilize=true"));
    CHECK(refusal({"--focused-phase-saving=both"})
              .starts_with("--focused-phase-saving requires --stabilize=true"));
    CHECK(restart_config(parse_with_input({"--stabilize=true", "--restart=lbd"})).method ==
          RestartMethod::lbd);
    CHECK(refusal({"--phase-saving=none", "--phase-saving-target-reset=10"})
              .starts_with("--phase-saving-target-reset requires"));
    CHECK_NOTHROW(parse_with_input({"--phase-saving=none", "--stabilize=true",
                                    "--focused-phase-saving=target-only",
                                    "--phase-saving-target-reset=10"}));
    CHECK_NOTHROW(parse_with_input({"--phase-saving=none", "--stabilize=true",
                                    "--focused-phase-saving=rotate",
                                    "--phase-rotate-starting-interval=10"}));
    CHECK_FALSE(csids_config(parse_with_input({})).phase_saving.focused_source.has_value());
    CHECK(csids_config(parse_with_input({"--stabilize=true"})).phase_saving.focused_source ==
          PhaseSaving::recent_only);

    const Options options = parse_with_input(
        {"--stabilize=true", "--stabilize-interval=50", "--focused-decay=0.75",
         "--focused-restart-lbd-fast=20", "--focused-restart-lbd-slow=1000",
         "--focused-restart-lbd-min-ratio=1.2", "--focused-restart-lbd-min-conflicts=1000",
         "--focused-restart-lbd-trail-ratio=1.4", "--phase-saving=rotate"});
    const StabilizeConfig stabilize = stabilize_config(options);
    CHECK(stabilize.interval_conflicts == 50);
    CHECK(stabilize.focused_restarts.method == RestartMethod::lbd);
    CHECK(stabilize.focused_restarts.lbd_fast_window == 20);
    CHECK(stabilize.focused_restarts.lbd_slow_window == 1000);
    CHECK(stabilize.focused_restarts.lbd_min_ratio == 1.2);
    CHECK(stabilize.focused_restarts.lbd_min_conflicts == 1000);
    CHECK(stabilize.focused_restarts.lbd_trail_ratio == 1.4);
    CHECK(csids_config(options).focused_decay == 0.75);
    CHECK(restart_config(options).method == RestartMethod::none);
}

TEST_CASE("options: --report-type takes the progress group names") {
    CHECK(parse_with_input({"--report-type=all"}).report_type == "all");
    CHECK(parse_with_input({"--report-type=search,deletion"}).report_type == "search,deletion");

    // The vocabulary is not in the table, so validation is what enforces it.
    const std::string message = rejection_message<CliError>({"--report-type=nope"});
    CHECK(message.find("--report-type='nope'") != std::string::npos);
    CHECK(message.find("default, search, deletion, propagation, all") != std::string::npos);
}

TEST_CASE("options: the recorded configuration is the whole table") {
    const std::vector<ConfigEntry> settings = option_settings(parse_with_input({}));

    // One entry per row, in table order, so a statistics row records the recipe.
    CHECK(settings.size() > 20);
    CHECK(settings.front().name == "equation_repr");
    CHECK(settings.back().name == "report_type");

    const auto value_of = [&settings](const std::string& name) {
        for (const ConfigEntry& entry : settings) {
            if (entry.name == name) {
                return entry.value;
            }
        }
        return std::string("<absent>");
    };
    CHECK(value_of("compact") == "true");
    CHECK(value_of("implication_graph_cadence") ==
          std::to_string(Options{}.implication_graph_cadence));
    CHECK(value_of("csids_decay") == format_real(Options{}.csids_decay));
    CHECK(value_of("phase_default") == "falsify");
    CHECK(value_of("phase_saving") == "rotate");
    // An unset path records as empty, not as the help text's "unset".
    CHECK(value_of("proof") == "");
}

TEST_CASE("options: a recorded value knows whether it is the default") {
    const auto entry_for = [](const std::vector<ConfigEntry>& settings, const std::string& name) {
        for (const ConfigEntry& candidate : settings) {
            if (candidate.name == name) {
                return candidate;
            }
        }
        FAIL("no such option: ", name);
        return ConfigEntry{};
    };

    const std::vector<ConfigEntry> defaults = option_settings(parse_with_input({}));
    for (const ConfigEntry& entry : defaults) {
        CHECK(entry.is_default);
    }

    const std::vector<ConfigEntry> changed =
        option_settings(parse_with_input({"--seed=7", "--minimize=false"}));
    CHECK(!entry_for(changed, "seed").is_default);
    CHECK(!entry_for(changed, "minimize").is_default);
    CHECK(entry_for(changed, "implication_graph_cadence").is_default);
}

TEST_CASE("options: an option nothing would read is refused, not ignored") {
    // One case per shape in the requirement table, so a row that stops firing
    // shows up here rather than as a silently ignored flag.
    const auto refusal = [](std::vector<std::string> arguments) {
        return rejection_message<CliError>(std::move(arguments));
    };
    CHECK(refusal({"--deletion=false", "--deletion-wait=8"}) ==
          "--deletion-wait requires --deletion=true; nothing would read it here");
    CHECK(refusal({"--restart-lbd-min-ratio=1.2"}).starts_with("--restart-lbd-min-ratio requires"));
    CHECK(refusal({"--restart-luby-unit=50"}).starts_with("--restart-luby-unit requires"));
    CHECK(refusal({"--deletion=false", "--deletion-cadence-coefficient=0.5"})
              .starts_with("--deletion-cadence-coefficient requires"));
    CHECK(refusal({"--deletion=false", "--deletion-cadence-offset=0"})
              .starts_with("--deletion-cadence-offset requires"));
    CHECK(refusal({"--stats-file=s.csv", "--report-type=all"})
              .starts_with("--report-type requires --stats-file unset and --report-cadence > 0"));
    CHECK(refusal({"--report-cadence=0", "--report-type=all"}).starts_with("--report-type requires"));
    CHECK(refusal({"--stats-file=s.csv", "--report-cadence=10"})
              .starts_with("--report-cadence requires --stats-file unset"));

    CHECK(parse_with_input({"--csids-intersection-allow-negation=true", "--phase-default=satisfy"})
              .phase_default == DecisionPhase::satisfy);
    CHECK(csids_config(parse_with_input({"--csids-intersection-allow-negation=true"})).span ==
          CsidsSpan::plus_minus);
    CHECK(csids_config(parse_with_input({})).span == CsidsSpan::plain);
    CHECK(refusal({"--phase-saving=recent-only", "--phase-saving-target-reset=10"})
              .starts_with("--phase-saving-target-reset requires "
                           "--phase-saving=both|target-only|rotate or "
                           "--focused-phase-saving=both|target-only|rotate"));
    CHECK_NOTHROW(parse_with_input({"--phase-saving=target-only", "--phase-saving-target-reset=10"}));
    CHECK(refusal({"--phase-saving=both", "--phase-rotate-starting-interval=10"})
              .starts_with("--phase-rotate-starting-interval requires --phase-saving=rotate or "
                           "--focused-phase-saving=rotate"));
    CHECK_NOTHROW(parse_with_input(
        {"--phase-rotate-starting-interval=10", "--csids-intersection-allow-negation=true"}));
    const Options coin_flip = parse_with_input({"--phase-saving=both", "--phase-default=random"});
    CHECK(coin_flip.phase_saving == PhaseSaving::both);
    CHECK(coin_flip.phase_default == DecisionPhase::random);
    CHECK(refusal({"--phase-default=saved"}).starts_with("--phase-default"));
    CHECK(refusal({"--phase-default=rotate"}).starts_with("--phase-default"));

    CHECK(refusal({"--implication-graph=false", "--implication-graph-min-new-clauses=8"})
              .starts_with("--implication-graph-min-new-clauses requires --implication-graph=true"));
    CHECK(parse_with_input({"--implication-graph-min-new-clauses=8"})
              .implication_graph_min_new_clauses == 8);
    CHECK(refusal({"--implication-graph=false", "--implication-graph-max-pairs=8"})
              .starts_with("--implication-graph-max-pairs requires --implication-graph=true"));
    CHECK(refusal({"--implication-graph=false", "--implication-graph-max-descendants=8"})
              .starts_with(
                  "--implication-graph-max-descendants requires --implication-graph=true"));
    CHECK(parse_with_input({"--implication-graph-max-pairs=8",
                            "--implication-graph-max-descendants=4"})
              .implication_graph_max_descendants == 4);
    CHECK_NOTHROW(parse_with_input({"--implication-graph=true", "--proof=p.lrup"}));

    // Each of these is given where something does read it.
    CHECK(parse_with_input({"--deletion-wait=8"}).deletion_wait == doctest::Approx(8.0));
    CHECK(parse_with_input({"--restart=lbd", "--restart-lbd-min-ratio=1.2"}).restart_lbd_min_ratio ==
          doctest::Approx(1.2));
    CHECK(refusal({"--selection=exact-watchlist", "--propagate-miss-rate=1"})
              .starts_with("--propagate-miss-rate requires --selection=cyclic"));
    CHECK(parse_with_input({"--selection=cyclic", "--propagate-miss-rate=0.5"})
              .propagate_miss_rate == doctest::Approx(0.5));
    CHECK(parse_with_input({"--report-type=all"}).report_type == "all");
}

TEST_CASE("options: only the cyclic selector reads the miss rate") {
    const auto cyclic = std::vector<std::string>{"--selection=cyclic"};
    CHECK(solver_config(parse_with_input(cyclic)).attempt_budget->miss_rate ==
          doctest::Approx(0.05));
    CHECK_FALSE(solver_config(parse_with_input({})).attempt_budget.has_value());

    std::vector<std::string> complete = cyclic;
    complete.emplace_back("--propagate-miss-rate=0");
    CHECK_FALSE(solver_config(parse_with_input(std::move(complete))).attempt_budget.has_value());
}

TEST_CASE("options: vivification runs under the cyclic budget") {
    const SolverConfig config =
        solver_config(parse_with_input({"--selection=cyclic", "--propagate-miss-rate=0.5"}));
    CHECK(config.attempt_budget.has_value());
    CHECK(config.vivify.has_value());
}

TEST_CASE("options: BVE and the implication graph go with renumbering on or off") {
    for (const std::string compact : {"--compact=false", "--compact=true"}) {
        CAPTURE(compact);
        CHECK(parse_with_input({compact}).implication_graph == true);
        CHECK(parse_with_input({compact}).bve == true);
        CHECK_NOTHROW(parse_with_input({compact, "--implication-graph=true", "--bve=true"}));
    }
    CHECK(parse_with_input({"--implication-graph=false"}).implication_graph == false);
}

} // namespace xorcle
