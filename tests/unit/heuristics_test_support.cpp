#include "heuristics_test_support.h"

#include "algebra_test_support.h"
#include "cli/options.h"

#include <string>
#include <utility>

namespace xorcle {

CsidsConfig default_csids_config() { return csids_config(Options{}); }

RestartConfig default_restart_config() { return restart_config(Options{}); }

ImplicationGraphConfig default_implication_graph_config() {
    return implication_graph_config(Options{});
}

BveConfig default_bve_config() { return bve_config(Options{}); }

DeletionConfig default_deletion_config() { return deletion_config(Options{}); }

SolverConfig default_solver_config() { return solver_config(Options{}); }

CsidsConfig csids_config_deciding(const DecisionPhase phase) {
    CsidsConfig config = default_csids_config();
    config.phase = phase;
    config.phase_saving.source = PhaseSaving::none;
    return config;
}

Clause make_clause(EquationRepr repr, std::size_t num_vars,
                   std::initializer_list<std::initializer_list<std::size_t>> rows) {
    Clause clause(repr, num_vars);
    for (const std::initializer_list<std::size_t>& row : rows) {
        clause.add_row(make_equation_with(repr, num_vars, row));
    }
    return clause;
}

RowSpace make_row_space(EquationRepr repr, std::size_t num_vars,
                        std::initializer_list<std::initializer_list<std::size_t>> rows) {
    RowSpace space(repr, num_vars);
    for (const std::initializer_list<std::size_t>& row : rows) {
        space.add_row(make_equation_with(repr, num_vars, row));
    }
    return space;
}

std::size_t dimensions_outside_span(const Clause& clause, const RowSpace& span) {
    RowSpace enlarged = span;
    std::size_t added = 0;
    for (const Equation& row : clause) {
        added += enlarged.add_row_get_row_idx(row).has_value() ? 1U : 0U;
    }
    return added;
}

void report_new_watch(WatchUpdateHandler& handler, ClauseId id, const Clause& clause,
                      const ClauseWatches& watches, ClauseRowIdx old_watch_row_idx, WatchType type,
                      const Trail& trail) {
    const RowSpace& span = trail.span();
    Equation first = clause.row(watches.watched0);
    Equation second = clause.row(watches.watched1);
    WatchReductions reductions;
    reductions.first_result = span.partial_reduce(first);
    reductions.second_result = span.partial_reduce_with_extra(second, first);
    reductions.first = type == WatchType::zero ? &first : nullptr;
    reductions.second = &second;
    handler.update_with_new_watch(id, old_watch_row_idx, type, trail, reductions);
}

ClauseId add_starting_clause(ClauseDatabase& database, ClauseSelector& selector, Clause clause) {
    const ClauseId id = database.add_starting_clause(std::move(clause));
    selector.update_with_new_clause(id);
    return id;
}

ClauseId add_asserting_clause(ClauseDatabase& database, ClauseSelector& selector, Clause clause,
                              const Trail& trail, const AssertingClauseInfo& info) {
    const ClauseId id = database.add_asserting_clause(std::move(clause), info.lbd);
    selector.update_with_asserting_clause(id, trail, info);
    return id;
}

void deactivate_clause(ClauseDatabase& database, ClauseSelector& selector, ClauseId id) {
    database.deactivate_clause(id);
    selector.update_with_inactive_clause(id);
}

void renumber(ClauseDatabase& database, ClauseSelector& selector,
              const CompactionTransform& transform, const Trail& trail) {
    database.renumber(transform);
    selector.update_with_renumbering(transform, trail);
}

std::string sign_pattern_instance(std::size_t num_vars) {
    const std::size_t num_clauses = std::size_t{1} << num_vars;
    std::string text =
        "p cnf " + std::to_string(num_vars) + " " + std::to_string(num_clauses) + "\n";
    for (std::size_t pattern = 0; pattern < num_clauses; ++pattern) {
        for (std::size_t var = 0; var < num_vars; ++var) {
            const bool negated = ((pattern >> (num_vars - 1 - var)) & 1U) != 0;
            text += (negated ? "-" : "") + std::to_string(var + 1) + " ";
        }
        text += "0\n";
    }
    return text;
}

} // namespace xorcle
