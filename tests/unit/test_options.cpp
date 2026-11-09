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
    CHECK(options.decisions == DecisionProcedureName::csids);
    CHECK(options.phase == DecisionPhase::saved);
    CHECK(options.selection == ClauseSelectorName::exact_watchlist);
    CHECK(options.restart == RestartMethod::none);
    CHECK(options.deletion == true);
    CHECK(options.deletion_key == DeletionKey::activity);
    CHECK(options.bump_responsible_clauses == false);
    CHECK(options.implication_graph == true);
    CHECK(options.proof.empty());
    CHECK(options.stats_file.empty());
    CHECK(options.report == "default");
}

TEST_CASE("options: --name=value sets each kind") {
    const Options options = parse_with_input({"--equation-repr=sparse",
                                              "--selection=cyclic",
                                              "--seed=42",
                                              "--compact=false",
                                              "--csids-initial=2.5",
                                              "--csids-decay=0.5",
                                              "--csids-bump=0",
                                              "--phase=satisfy",
                                              "--deletion=false",
                                              "--subsume=false",
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
    CHECK(options.deletion == false);
    CHECK(options.subsume == false);
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
    const std::string message = rejection_message<CliError>({"--phase=unsat"});
    CHECK(contains(message, "--phase='unsat' is not one of"));
    CHECK(contains(message, "falsify|satisfy|random"));
    CHECK(contains(rejection_message<CliError>({"--phase=sat"}), "is not one of"));
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

TEST_CASE("options: --proof and --proof-raw go with compaction on or off") {
    const Options options = parse_with_input({"--proof=out.lrup"});
    CHECK(options.proof == "out.lrup");
    CHECK(options.proof_raw.empty());
    CHECK(parse_with_input({"--proof=out.lrup", "--compact=false"}).proof == "out.lrup");

    const Options raw = parse_with_input({"--proof-raw=out.raw"});
    CHECK(raw.proof.empty());
    CHECK(raw.proof_raw == "out.raw");
    CHECK(parse_with_input({"--proof-raw=out.raw", "--compact=false"}).proof_raw == "out.raw");
}

TEST_CASE("options: enum labels are the first spelling naming an enumerator") {
    CHECK(enum_option_label("decisions", static_cast<int>(DecisionProcedureName::csids)) == "csids");
    CHECK(enum_option_label("phase", static_cast<int>(DecisionPhase::falsify)) == "falsify");
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
    CHECK(contains(text, "--decisions=csids|fixed_literals"));
    CHECK(contains(text, "--phase=falsify|satisfy|random|saved"));
    CHECK(contains(text, "--selection=cyclic|exact_watchlist"));
    CHECK(contains(text, "--proof=PATH"));
    CHECK(contains(text, "--stats-file=PATH"));
    CHECK(contains(text, "--help, -h"));
    CHECK(contains(text, "--version"));

    CHECK(contains(text, "--deletion=true|false"));
    CHECK(contains(text, "periodically delete some learned clauses [default: true]"));
    CHECK(contains(text, "--deletion-cadence-coefficient=X"));
    CHECK(contains(text, "--deletion-cadence-offset=N"));
    CHECK(contains(text, "--deletion-lbd-window=N"));
    CHECK(contains(text, "--deletion-eviction-rate=X"));
    CHECK(contains(text, "--deletion-wait=X"));
    CHECK(contains(text, "--deletion-key=lbd|activity"));

    // The requirement table feeds the help, so a refusal is never a surprise.
    CHECK(contains(text, "plus --deletion-cadence-offset (positive); requires --deletion=true"));
    CHECK(contains(text, "fast/slow ratio needed for LBD restart; requires --restart=lbd"));

    CHECK(contains(text, "--restart=none|luby|lbd"));
    CHECK(contains(text, "restart schedule"));
    CHECK(contains(text, "conflicts for a luby value of 1"));
    CHECK(contains(text, "window of the fast LBD average"));
}

TEST_CASE("options: the luby budget is a positive count") {
    CHECK(contains(rejection_message<CliError>({"--restart=luby", "--restart-luby-unit=0"}),
                   "--restart-luby-unit"));
    CHECK_NOTHROW(parse_with_input({"--restart=luby"}));
    CHECK_NOTHROW(parse_with_input({"--restart=luby", "--restart-luby-unit=4000"}));
}


TEST_CASE("options: the exact watchlist takes deletion and subsumption, but not sparse rows") {
    const Options options = parse_with_input({"--selection=exact_watchlist"});
    CHECK(options.deletion);
    CHECK(options.subsume);
    CHECK(propagation_config(options).miss_rate == 0.0);
    CHECK(rejection_message<CliError>({"--selection=exact_watchlist", "--equation-repr=sparse"})
              .starts_with("--selection=exact_watchlist requires --equation-repr=dense"));
    CHECK(rejection_message<CliError>({"--equation-repr=sparse"})
              .starts_with("--selection=exact_watchlist requires --equation-repr=dense"));
    CHECK(parse_with_input({"--selection=cyclic", "--equation-repr=sparse"}).equation_repr ==
          EquationRepr::sparse);
}

TEST_CASE("options: deletion refuses an incompatible choice rather than yielding") {
    CHECK(contains(rejection_message<CliError>({"--deletion-cadence-coefficient=0"}),
                   "--deletion-cadence-coefficient must be positive"));
    CHECK(contains(rejection_message<CliError>({"--deletion-lbd-window=0"}), "must be at least 1"));
    CHECK_NOTHROW(parse_with_input({"--deletion-lbd-window=1"}));
    CHECK(contains(rejection_message<CliError>({"--deletion-eviction-rate=1.5"}),
                   "--deletion-eviction-rate"));
    CHECK_NOTHROW(parse_with_input({"--deletion-eviction-rate=0"}));
    CHECK_NOTHROW(parse_with_input({"--deletion-eviction-rate=1"}));

    // The key defaults to activity, so the default deletion alone carries the
    // requirement, whether or not --deletion is given.
    CHECK(rejection_message<CliError>({"--decisions=fixed_literals"})
              .starts_with("--deletion-key=activity requires --decisions=csids"));
    CHECK(contains(rejection_message<CliError>({"--deletion=true", "--decisions=fixed_literals"}),
                   "requires --decisions=csids"));

    // Saying what you want is always accepted.
    CHECK(parse_with_input({"--decisions=fixed_literals", "--deletion=false"}).deletion == false);
    CHECK(parse_with_input({"--decisions=fixed_literals", "--deletion-key=lbd"}).deletion == true);
    CHECK_NOTHROW(parse_with_input({"--deletion=true", "--deletion-key=activity"}));

    // A compatible choice needs nothing said.
    CHECK(parse_with_input({}).deletion == true);
    CHECK(parse_with_input({"--selection=cyclic"}).deletion == true);
}

TEST_CASE("options: each configuration struct defaults to what an empty command line gives") {
    const Options defaults = parse_with_input({});
    CHECK(csids_config(defaults) == CsidsConfig{});
    CHECK(compaction_config(defaults) == CompactionConfig{});
    CHECK(deletion_config(defaults) == DeletionConfig{});
    CHECK(implication_graph_config(defaults) == ImplicationGraphConfig{});
    // `--propagate-miss-rate` is read only under `--selection=cyclic`, so the
    // default selector resolves it to 0.
    CHECK(propagation_config(defaults) == PropagationConfig{});
    CHECK(restart_config(defaults) == RestartConfig{});
}

TEST_CASE("options: the configuration projections carry the values across") {
    // Each option is given where something reads it: the LBD ratio needs the LBD
    // schedule, so the Luby settings are checked in their own parse below.
    const Options options = parse_with_input({"--compact=false",
                                              "--restart=lbd", "--restart-lbd-min-ratio=1.25",
                                              "--deletion=true",
                                              "--deletion-cadence-coefficient=0.75",
                                              "--deletion-cadence-offset=250",
                                              "--deletion-lbd-window=4",
                                              "--deletion-eviction-rate=0.25",
                                              "--deletion-wait=7.5",
                                              "--deletion-key=activity"});
    const DeletionConfig deletion = deletion_config(options);
    CHECK(deletion.enabled == true);
    CHECK(deletion.cadence_coefficient == doctest::Approx(0.75));
    CHECK(deletion.cadence_offset == 250);
    CHECK(deletion.lbd_window == 4);
    CHECK(deletion.eviction_rate == doctest::Approx(0.25));
    CHECK(deletion.wait == doctest::Approx(7.5));
    CHECK(deletion.key == DeletionKey::activity);

    const CompactionConfig compaction = compaction_config(options);
    CHECK(compaction.enabled == false);

    const RestartConfig restart = restart_config(options);
    CHECK(restart.method == RestartMethod::lbd);
    CHECK(restart.lbd_min_ratio == doctest::Approx(1.25));

    const RestartConfig luby =
        restart_config(parse_with_input({"--restart=luby", "--restart-luby-unit=50"}));
    CHECK(luby.method == RestartMethod::luby);
    CHECK(luby.luby_unit_conflicts == 50);

    CHECK(compaction_config(parse_with_input({})).enabled == true);
    CHECK(compaction_config(parse_with_input({"--compact=false", "--implication-graph=false"}))
              .enabled == false);
}

TEST_CASE("options: --report takes the progress group names") {
    CHECK(parse_with_input({"--report=all"}).report == "all");
    CHECK(parse_with_input({"--report=search,deletion"}).report == "search,deletion");

    // The vocabulary is not in the table, so validation is what enforces it.
    const std::string message = rejection_message<CliError>({"--report=nope"});
    CHECK(message.find("--report='nope'") != std::string::npos);
    CHECK(message.find("default, search, deletion, propagation, all") != std::string::npos);
}

TEST_CASE("options: the recorded configuration is the whole table") {
    const std::vector<ConfigEntry> settings = option_settings(parse_with_input({}));

    // One entry per row, in table order, so a statistics row records the recipe.
    CHECK(settings.size() > 20);
    CHECK(settings.front().name == "equation_repr");
    CHECK(settings.back().name == "report");

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
    CHECK(value_of("decisions") == "csids");
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
    CHECK(refusal({"--decisions=fixed_literals", "--deletion=false", "--csids-decay=0.5"})
              .starts_with("--csids-decay requires --decisions=csids"));
    CHECK(refusal({"--restart-lbd-min-ratio=1.2"}).starts_with("--restart-lbd-min-ratio requires"));
    CHECK(refusal({"--restart-luby-unit=50"}).starts_with("--restart-luby-unit requires"));
    CHECK(refusal({"--deletion=false", "--deletion-cadence-coefficient=0.5"})
              .starts_with("--deletion-cadence-coefficient requires"));
    CHECK(refusal({"--deletion=false", "--deletion-cadence-offset=0"})
              .starts_with("--deletion-cadence-offset requires"));
    // Only CSIDS scores clauses, so only CSIDS can bump the pivots.
    CHECK(refusal({"--decisions=fixed_literals", "--deletion=false",
                   "--bump-responsible-clauses=true"})
              .starts_with("--bump-responsible-clauses requires --decisions=csids"));
    CHECK(refusal({"--stats-file=s.csv", "--report=all"}).starts_with("--report requires"));
    CHECK(refusal({"--report-cadence=0", "--report=all"}).starts_with("--report requires"));

    // --phase reaches only CSIDS; --seed is read either way.
    CHECK(refusal({"--decisions=fixed_literals", "--deletion=false", "--phase=satisfy"})
              .starts_with("--phase requires --decisions=csids"));
    CHECK_NOTHROW(
        parse_with_input({"--decisions=fixed_literals", "--deletion=false", "--seed=7"}));
    CHECK(refusal({"--decisions=fixed_literals", "--deletion=false", "--csids-span=plus_minus"})
              .starts_with("--csids-span requires --decisions=csids"));
    CHECK(parse_with_input({"--csids-span=plus_minus", "--phase=satisfy"}).phase ==
          DecisionPhase::satisfy);
    CHECK(parse_with_input({"--csids-span=plus_minus"}).csids_span == CsidsSpan::plus_minus);
    CHECK(refusal({"--phase=falsify", "--phase-saving-recent=2"})
              .starts_with("--phase-saving-recent requires"));
    CHECK(refusal({"--phase=saved", "--phase-saving-target=false",
                   "--phase-saving-target-reset=10"})
              .starts_with("--phase-saving-target-reset requires"));
    CHECK_NOTHROW(parse_with_input({"--phase=saved", "--phase-saving-recent=2"}));
    CHECK(refusal({"--phase=saved", "--phase-saving-target=false",
                   "--phase-saving-target-on-ties=true"})
              .starts_with("--phase-saving-target-on-ties requires"));
    CHECK(parse_with_input({"--csids-span=plus_minus", "--phase=saved"}).phase ==
          DecisionPhase::saved);
    CHECK(parse_with_input({"--phase=saved"}).phase == DecisionPhase::saved);
    CHECK_NOTHROW(
        parse_with_input({"--restart=lbd", "--decisions=fixed_literals", "--deletion=false"}));

    // So is the implication graph, whose units only stop steering the search
    // once a compaction rewrites the database against them.
    CHECK(refusal({"--implication-graph=true", "--compact=false"})
              .starts_with("--implication-graph requires --compact=true"));
    CHECK(refusal({"--implication-graph=false", "--implication-graph-clause-cadence=8"})
              .starts_with("--implication-graph-clause-cadence requires --implication-graph=true"));
    CHECK(parse_with_input({"--implication-graph-clause-cadence=8"})
              .implication_graph_clause_cadence == 8);
    CHECK(refusal({"--implication-graph=false", "--implication-graph-max-pairs=8"})
              .starts_with("--implication-graph-max-pairs requires --implication-graph=true"));
    CHECK(refusal({"--implication-graph=false", "--implication-graph-max-descendants=8"})
              .starts_with(
                  "--implication-graph-max-descendants requires --implication-graph=true"));
    CHECK(parse_with_input({"--implication-graph-max-pairs=8",
                            "--implication-graph-max-descendants=4"})
              .implication_graph_max_descendants == 4);
    CHECK_NOTHROW(parse_with_input({"--implication-graph=false", "--compact=false"}));
    CHECK_NOTHROW(parse_with_input({"--implication-graph=true", "--proof=p.lrup"}));
    CHECK_NOTHROW(parse_with_input({"--implication-graph=true", "--proof-raw=p.raw"}));

    // Each of these is given where something does read it.
    CHECK(parse_with_input({"--deletion-wait=8"}).deletion_wait == doctest::Approx(8.0));
    CHECK(parse_with_input({"--restart=lbd", "--restart-lbd-min-ratio=1.2"}).restart_lbd_min_ratio ==
          doctest::Approx(1.2));
    CHECK(refusal({"--selection=exact_watchlist", "--propagate-miss-rate=1"})
              .starts_with("--propagate-miss-rate requires --selection=cyclic"));
    CHECK(parse_with_input({"--selection=cyclic", "--propagate-miss-rate=0.5"})
              .propagate_miss_rate == doctest::Approx(0.5));
    CHECK(refusal({"--selection=cyclic", "--propagate-miss-rate=0", "--propagate-window=500"})
              .starts_with("--propagate-window requires --selection=cyclic and a nonzero "
                           "--propagate-miss-rate"));
    CHECK(refusal({"--propagate-window=500"})
              .starts_with("--propagate-window requires --selection=cyclic and a nonzero "
                           "--propagate-miss-rate"));
    CHECK(parse_with_input({"--selection=cyclic", "--propagate-miss-rate=0.02",
                            "--propagate-window=500"})
              .propagate_window == 500);
    CHECK(parse_with_input({"--report=all"}).report == "all");
}

TEST_CASE("options: only the cyclic selector reads the miss rate") {
    const auto cyclic = std::vector<std::string>{"--selection=cyclic"};
    CHECK(propagation_config(parse_with_input(cyclic)).miss_rate == doctest::Approx(0.05));
    CHECK(propagation_config(parse_with_input({})).miss_rate == 0.0);

    std::vector<std::string> complete = cyclic;
    complete.emplace_back("--propagate-miss-rate=0");
    CHECK(propagation_config(parse_with_input(std::move(complete))).miss_rate == 0.0);
}

TEST_CASE("options: the default implication graph follows compaction") {
    CHECK(parse_with_input({}).implication_graph == true);
    CHECK(parse_with_input({"--compact=false"}).implication_graph == false);
    CHECK(implication_graph_config(parse_with_input({"--compact=false"})).enabled == false);

    CHECK(rejection_message<CliError>({"--compact=false", "--implication-graph=true"})
              .starts_with("--implication-graph requires --compact=true"));

    CHECK(parse_with_input({"--implication-graph=false"}).implication_graph == false);
}

} // namespace xorcle
