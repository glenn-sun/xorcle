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
    CHECK(options.deletion == true);
    CHECK(options.deletion_every == 100);
    CHECK(options.deletion_core_lbd == 2);
    CHECK(options.deletion_tier2_lbd == 6);
    CHECK(options.deletion_tier2_demote_every == 200);
    CHECK(options.deletion_key == DeletionKey::activity);
    CHECK(options.bump_learned_clause_pivots == false);
    CHECK(options.proof.empty());
    CHECK(options.stats_file.empty());
    CHECK(options.report_every == 100);
    CHECK(options.report == "default");
    CHECK(options.csids_preset.empty());
}

TEST_CASE("options: --name=value sets each kind") {
    // `--deletion=false` because the selector cannot delete, and
    // `--restart-reuse-trail=false` because a Luby restart would otherwise want
    // levels only CSIDS ranks; both are now refusals rather than quiet demotions.
    const Options options = parse_with_input({"--equation-repr=sparse",
                                              "--seed=42",
                                              "--compact=false",
                                              "--csids-initial=2.5",
                                              "--csids-decay=0.5",
                                              "--csids-bump=0",
                                              "--phase=satisfy",
                                              "--selection=leading_term_watchlist",
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
    CHECK(contains(rejection_message<CliError>({"--selection=exact_watchlist"}), "is not one of"));
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

TEST_CASE("options: --proof goes with compaction, and --proof-raw with either") {
    // Post-processing derives the units compaction ate, so the two go together.
    const Options options = parse_with_input({"--proof=out.lrup"});
    CHECK(options.compact);
    CHECK(options.proof == "out.lrup");
    CHECK(options.proof_raw.empty());

    const Options raw = parse_with_input({"--proof-raw=out.raw"});
    CHECK(raw.proof.empty());
    CHECK(raw.proof_raw == "out.raw");
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

    CHECK(contains(text, "--deletion=true|false"));
    CHECK(contains(text, "periodically delete some learned clauses (only with cyclic selector) "
                       "[default: true]"));
    CHECK(contains(text, "--deletion-every=N"));
    CHECK(contains(text, "--deletion-key=lbd|activity"));

    // The requirement table feeds the help, so a refusal is never a surprise.
    CHECK(contains(text, "number of conflicts between compactions; requires --compact=true"));
    CHECK(contains(text, "fast/slow ratio needed for LBD restart; requires --restart=lbd"));

    CHECK(contains(text, "--restart=none|luby|lbd"));
    CHECK(contains(text, "restart schedule"));
    CHECK(contains(text, "multiplier for luby schedule"));
    CHECK(contains(text, "window of the fast LBD average"));
}

TEST_CASE("options: luby refuses to invent a budget") {
    CHECK(contains(rejection_message<CliError>({"--restart=luby"}),
                   "requires --restart-luby-unit"));
    // The budget is only required by the schedule that uses it.
    CHECK_NOTHROW(parse_with_input({"--restart=lbd"}));
    CHECK_NOTHROW(parse_with_input({"--restart=luby", "--restart-luby-unit=4000"}));
}

TEST_CASE("options: deletion needs the heuristics it reads") {
    CHECK(contains(
        rejection_message<CliError>({"--deletion=true", "--selection=leading_term_watchlist"}),
        "--deletion requires --selection=cyclic"));
    CHECK(contains(rejection_message<CliError>({"--deletion=true", "--deletion-key=activity",
                                                "--decisions=variable_vsids"}),
                   "--deletion-key=activity requires --decisions=csids"));
    CHECK(contains(rejection_message<CliError>({"--deletion-core-lbd=7"}),
                   "--deletion-core-lbd must not exceed --deletion-tier2-lbd"));

    // The key defaults to activity, so --deletion alone carries the requirement.
    const std::string defaulted =
        rejection_message<CliError>({"--deletion=true", "--decisions=variable_vsids"});
    CHECK(contains(defaulted, "requires --decisions=csids"));
    CHECK(contains(rejection_message<CliError>({"--deletion=true", "--decisions=fixed_literals"}),
                   "requires --decisions=csids"));

    // Each constraint binds only where deletion actually reads the heuristic.
    CHECK_NOTHROW(parse_with_input({"--deletion=true", "--deletion-key=activity"}));
    CHECK_NOTHROW(parse_with_input(
        {"--deletion=true", "--decisions=variable_vsids", "--deletion-key=lbd"}));
    CHECK_NOTHROW(parse_with_input({"--deletion-core-lbd=7", "--deletion-tier2-lbd=9"}));
}

TEST_CASE("options: deletion refuses an incompatible choice rather than yielding") {
    // It used to switch itself off, which made a bare --selection a quietly
    // different experiment. Nothing in the option layer resolves silently now.
    const auto refusal = [](std::vector<std::string> arguments) {
        return rejection_message<CliError>(std::move(arguments));
    };
    CHECK(refusal({"--selection=leading_term_watchlist"}).find("--deletion requires") == 0);
    CHECK(refusal({"--decisions=variable_vsids"}).find("--deletion-key=activity") == 0);
    CHECK(refusal({"--decisions=fixed_literals"}).find("--deletion-key=activity") == 0);

    // Saying what you want is always accepted.
    CHECK(parse_with_input({"--selection=leading_term_watchlist", "--deletion=false",
                            "--subsume=false"}).deletion == false);
    CHECK(parse_with_input({"--decisions=variable_vsids", "--deletion=false"}).deletion == false);
    CHECK(parse_with_input({"--decisions=variable_vsids", "--deletion-key=lbd"}).deletion == true);

    // A compatible choice needs nothing said.
    CHECK(parse_with_input({}).deletion == true);
    CHECK(parse_with_input({"--selection=cyclic"}).deletion == true);
    CHECK(parse_with_input({"--decisions=berkmin"}).deletion == true);
}

TEST_CASE("options: the configuration projections carry the values across") {
    // Each option is given where something reads it: the LBD ratio needs the LBD
    // schedule, so the Luby settings are checked in their own parse below.
    const Options options = parse_with_input({"--compact=false",
                                              "--restart=lbd", "--restart-lbd-min-ratio=1.25",
                                              "--deletion=true",
                                              "--deletion-every=25", "--deletion-core-lbd=2",
                                              "--deletion-tier2-lbd=8", "--deletion-key=activity",
                                              "--deletion-tier2-demote-every=75"});
    const DeletionConfig deletion = deletion_config(options);
    CHECK(deletion.enabled == true);
    CHECK(deletion.conflict_interval == 25);
    CHECK(deletion.demote_interval == 75);
    CHECK(deletion.tiers.core_lbd == 2);
    CHECK(deletion.tiers.tier2_lbd == 8);
    CHECK(deletion.key == DeletionKey::activity);
    // Only the caller holding the decision procedure can fill this in.
    CHECK(deletion.activity == nullptr);

    const CompactionConfig compaction = compaction_config(options);
    CHECK(compaction.enabled == false);

    const RestartConfig restart = restart_config(options);
    CHECK(restart.method == RestartMethod::lbd);
    CHECK(restart.lbd_min_ratio == doctest::Approx(1.25));

    const RestartConfig luby =
        restart_config(parse_with_input({"--restart=luby", "--restart-luby-unit=50"}));
    CHECK(luby.method == RestartMethod::luby);
    CHECK(luby.luby_unit_conflicts == 50);

    const CompactionConfig interval =
        compaction_config(parse_with_input({"--compact-every=8"}));
    CHECK(interval.enabled == true);
    CHECK(interval.conflict_interval == 8);
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
    CHECK(value_of("compact_every") == "16");
    CHECK(value_of("csids_decay") == "0.98");
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
    CHECK(entry_for(changed, "compact_every").is_default);
}

TEST_CASE("options: a preset records as the triple it stands for") {
    // Replaying the record must reproduce the run, so the preset resolves rather
    // than being recorded under its own name.
    const std::vector<ConfigEntry> settings = option_settings(parse_with_input({"--decisions=cmtf"}));
    const auto value_of = [&settings](const std::string& name) {
        for (const ConfigEntry& entry : settings) {
            if (entry.name == name) {
                return entry.value;
            }
        }
        return std::string("<absent>");
    };
    CHECK(value_of("decisions") == "csids");
    CHECK(value_of("csids_initial") != "<absent>");
    CHECK(value_of("csids_decay") != "<absent>");
}

TEST_CASE("options: an option nothing would read is refused, not ignored") {
    // One case per shape in the requirement table, so a row that stops firing
    // shows up here rather than as a silently ignored flag.
    const auto refusal = [](std::vector<std::string> arguments) {
        return rejection_message<CliError>(std::move(arguments));
    };
    CHECK(refusal({"--compact=false", "--compact-every=8"}) ==
          "--compact-every requires --compact=true; nothing would read it here");
    CHECK(refusal({"--decisions=variable_vsids", "--deletion=false", "--csids-decay=0.5"})
              .starts_with("--csids-decay requires --decisions=csids"));
    CHECK(refusal({"--wait-for-queue=4"}).starts_with("--wait-for-queue requires"));
    CHECK(refusal({"--restart-lbd-min-ratio=1.2"}).starts_with("--restart-lbd-min-ratio requires"));
    CHECK(refusal({"--restart-luby-unit=50"}).starts_with("--restart-luby-unit requires"));
    CHECK(refusal({"--deletion=false", "--deletion-every=25"})
              .starts_with("--deletion-every requires"));
    CHECK(refusal({"--deletion=false", "--deletion-tier2-demote-every=25"})
              .starts_with("--deletion-tier2-demote-every requires"));
    // Only CSIDS scores clauses, so only CSIDS can bump the pivots.
    CHECK(refusal({"--decisions=variable_vsids", "--deletion=false",
                   "--bump-learned-clause-pivots=true"})
              .starts_with("--bump-learned-clause-pivots requires --decisions=csids"));
    CHECK(refusal({"--stats-file=s.csv", "--report=all"}).starts_with("--report requires"));
    CHECK(refusal({"--report-every=0", "--report=all"}).starts_with("--report requires"));

    // The two that were inert while claiming to be on: --phase and --seed reach
    // only CSIDS, and trail reuse needs the procedure that ranks levels.
    CHECK(refusal({"--decisions=variable_vsids", "--deletion=false", "--phase=satisfy"})
              .starts_with("--phase requires --decisions=csids"));
    CHECK(refusal({"--decisions=fixed_literals", "--deletion=false", "--seed=7"})
              .starts_with("--seed requires --decisions=csids"));
    CHECK(refusal({"--restart=lbd", "--decisions=variable_vsids", "--deletion=false"})
              .starts_with("--restart-reuse-trail requires --decisions=csids"));

    // Trail reuse is moot without a schedule, so saying so is refused too; left
    // at its default under --restart=none it is simply not read.
    CHECK(refusal({"--restart-reuse-trail=false"}).starts_with("--restart-reuse-trail requires"));
    CHECK(parse_with_input({}).restart_reuse_trail == true);

    // Each of these is given where something does read it.
    CHECK(parse_with_input({"--compact-every=8"}).compact_every == 8);
    CHECK(parse_with_input({"--restart=lbd", "--restart-lbd-min-ratio=1.2"}).restart_lbd_min_ratio ==
          doctest::Approx(1.2));
    CHECK(parse_with_input({"--selection=leading_term_watchlist", "--deletion=false",
                            "--subsume=false", "--wait-for-queue=4"}).wait_for_queue == 4);
    CHECK(parse_with_input({"--report=all"}).report == "all");
}

} // namespace xorcle
