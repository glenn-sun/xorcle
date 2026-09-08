#include "io/proof_postprocess.h"

#include <cassert>
#include <cstddef>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>
#include <variant>
#include <vector>

#include "algebra/equation.h"
#include "algebra/row_echelon_matrix.h"
#include "util/index_set.h"

namespace xorcle {

namespace {

constexpr std::size_t no_index = std::numeric_limits<std::size_t>::max();

/// @brief A line's own id in the post-processor's space: a solver clause id
/// below `unit_node_base`, or `unit_node_base` plus a derived unit's index.
using NodeId = std::size_t;

/// @brief Above every clause id a clause store can hand out, so one lookup
/// covers clauses and units alike.
constexpr NodeId unit_node_base = std::size_t{1} << 48;

/// @brief One line the proof may keep: the clause it proves and its reasons.
struct Derivation {
    NodeId node;
    RawRow row; ///< The proof row of a unit line; unused for the other lines.
    std::vector<NodeId> reasons;
};

/// @brief A root unit compaction eliminated.
struct Unit {
    NodeId node;               ///< Its own node, or the aliased clause's.
    std::size_t derivation;    ///< `no_index` when it is an alias.
};

/// @brief A clause's proof rows, carried forward to the epoch it was last used in.
struct ClauseCache {
    std::vector<RawRow> rows;
    std::vector<std::size_t> units; ///< The units bringing `rows` to `epoch` used.
    std::size_t epoch = 0;
    std::size_t derivation = no_index;
    bool known = false;
};

/// @brief Turns a raw proof into an LRUP(⊕) proof in two passes over the source.
class Postprocessor {
public:
    Postprocessor(std::size_t num_vars, std::size_t num_input_clauses, EquationRepr repr,
                  std::span<const std::vector<RawRow>> input_clause_rows)
        : num_vars_(num_vars),
          num_input_clauses_(num_input_clauses),
          repr_(repr),
          unit_of_col_(num_vars + 1, no_index),
          scratch_(make_equation(repr, num_vars)) {
        assert(input_clause_rows.size() == num_input_clauses &&
               "postprocess_raw_proof: one row list per input clause");
        clauses_.resize(num_input_clauses);
        for (std::size_t id = 0; id < num_input_clauses; ++id) {
            clauses_[id].rows = input_clause_rows[id];
            clauses_[id].known = true;
        }
    }

    PostprocessSummary run(RawRecordSource& source, std::ostream& out) {
        scan(source);
        trim();
        emit(source, out);
        return summary_;
    }

private:
    // -- Pass 1: derive the unit lines and every final reason list. ----------

    void scan(RawRecordSource& source) {
        source.rewind();
        RawRecord record;
        while (source.next(record)) {
            if (const auto* learned = std::get_if<RawLearned>(&record)) {
                scan_learned(*learned);
            } else if (const auto* compaction = std::get_if<RawCompaction>(&record)) {
                scan_compaction(*compaction);
            } else if (const auto* empty = std::get_if<RawEmpty>(&record)) {
                scan_empty(*empty);
            } else {
                // Only the rows go: the line the clause proves stays reachable.
                for (const ClauseId id : std::get<RawDeleted>(record).ids) {
                    if (id < clauses_.size()) {
                        clauses_[id].rows = std::vector<RawRow>{};
                        clauses_[id].units = std::vector<std::size_t>{};
                        clauses_[id].known = false;
                    }
                }
            }
        }
        if (empty_derivation_ == no_index) {
            throw std::runtime_error("the raw proof has no empty clause");
        }
    }

    void scan_learned(const RawLearned& learned) {
        if (learned.id < num_input_clauses_) {
            throw std::runtime_error("the raw proof learns a clause with an input clause's id");
        }
        ++summary_.raw_learned;
        std::vector<NodeId> reasons = final_reasons(learned.reasons);
        ClauseCache& cache = clause(learned.id);
        cache.rows = learned.rows;
        cache.units.clear();
        cache.epoch = epochs_.size();
        cache.known = true;
        cache.derivation = add_derivation(learned.id, RawRow{}, std::move(reasons));
    }

    void scan_empty(const RawEmpty& empty) {
        if (empty_derivation_ != no_index) {
            throw std::runtime_error("the raw proof has more than one empty clause");
        }
        ++summary_.raw_learned;
        std::vector<NodeId> reasons = final_reasons(empty.reasons);
        ClauseCache& cache = clause(empty.id);
        cache.known = true;
        cache.derivation = add_derivation(empty.id, RawRow{}, std::move(reasons));
        empty_derivation_ = cache.derivation;
    }

    /// @brief The reasons a compacted epoch's line needs: each raw reason, with
    /// the units it propagates modulo inserted just before it.
    std::vector<NodeId> final_reasons(const std::vector<ClauseId>& raw_reasons) {
        std::vector<NodeId> reasons;
        ++line_stamp_;
        for (const ClauseId id : raw_reasons) {
            if (clause(id).derivation == no_index && id >= num_input_clauses_) {
                throw std::runtime_error("the raw proof names an unknown reason clause");
            }
            if (!epochs_.empty()) {
                for (const std::size_t unit : bring_forward(id).units) {
                    if (line_stamps_[unit] == line_stamp_) {
                        continue;
                    }
                    line_stamps_[unit] = line_stamp_;
                    reasons.push_back(units_[unit].node);
                    ++summary_.inserted_units;
                }
            }
            reasons.push_back(id);
        }
        return reasons;
    }

    void scan_compaction(const RawCompaction& compaction) {
        RowSpace echelon(repr_, num_vars_);
        for (const RawUnit& unit : compaction.units) {
            derive_unit(unit, echelon);
        }
        epochs_.push_back(std::move(echelon));
    }

    /// @brief Prove one root unit from its reason clause and the units before it.
    void derive_unit(const RawUnit& raw, RowSpace& echelon) {
        const ClauseCache& cache = bring_forward(raw.reason);
        std::vector<RawRow> rows = cache.rows;
        std::vector<std::size_t> same_epoch;
        reduce_rows(rows, echelon, same_epoch);

        // A trail row is reduced only on its leading column, so the unit has to
        // be brought all the way down before it can be compared, or written.
        Equation unit_row = make_equation(repr_, num_vars_);
        unit_row.assign_bits(raw.row);
        echelon.reduce(unit_row);

        // The self-check of the whole design: the reason clause must now
        // propagate exactly this unit, in its linear negation.
        const RawRow negated = negate(unit_row.bits());
        bool propagates = false;
        for (const RawRow& row : rows) {
            if (row.empty()) {
                continue;
            }
            if (row != negated) {
                throw std::runtime_error("a root unit's reason clause does not propagate it");
            }
            propagates = true;
        }
        if (!propagates) {
            throw std::runtime_error("a root unit's reason clause is already a conflict");
        }

        // An alias: the reason clause is already the unit line, so it needs none.
        const bool alias =
            cache.units.empty() && same_epoch.empty() && rows.size() == 1;
        Unit unit{raw.reason, no_index};
        if (!alias) {
            unit.node = unit_node_base + units_.size();
            std::vector<NodeId> reasons;
            for (const std::size_t index : cache.units) {
                reasons.push_back(units_[index].node);
            }
            for (const std::size_t index : same_epoch) {
                reasons.push_back(units_[index].node);
            }
            reasons.push_back(raw.reason);
            unit.derivation = add_derivation(unit.node, negated, std::move(reasons));
        }

        const std::size_t index = units_.size();
        units_.push_back(unit);
        unit_stamps_.push_back(0);
        line_stamps_.push_back(0);

        const std::optional<std::size_t> row_index =
            echelon.add_row_get_index(std::move(unit_row));
        if (!row_index) {
            throw std::runtime_error("a root unit repeats one already eliminated");
        }
        const std::optional<std::size_t> pivot = echelon.row(*row_index).leading_bit();
        assert(pivot && "a stored echelon row is never zero");
        unit_of_col_[*pivot] = index;
    }

    /// @brief Reduce a clause's rows by every epoch echelon it has not met yet.
    const ClauseCache& bring_forward(ClauseId id) {
        ClauseCache& cache = clause(id);
        if (!cache.known) {
            throw std::runtime_error("the raw proof names an unknown reason clause");
        }
        for (; cache.epoch < epochs_.size(); ++cache.epoch) {
            reduce_rows(cache.rows, epochs_[cache.epoch], cache.units);
        }
        return cache;
    }

    /// @brief Reduce `rows` in place, appending the units `echelon` used to `used`.
    void reduce_rows(std::vector<RawRow>& rows, const RowSpace& echelon,
                     std::vector<std::size_t>& used) {
        ++unit_stamp_;
        for (RawRow& row : rows) {
            scratch_.assign_bits(row);
            const IndexSet coefficients = echelon.reduce_get_coefficients(scratch_);
            coefficients.for_each_set([&](std::size_t row_index) {
                const std::optional<std::size_t> pivot = echelon.row(row_index).leading_bit();
                assert(pivot && "a stored echelon row is never zero");
                const std::size_t unit = unit_of_col_[*pivot];
                assert(unit != no_index && "an echelon pivot column names its unit");
                if (unit_stamps_[unit] != unit_stamp_) {
                    unit_stamps_[unit] = unit_stamp_;
                    used.push_back(unit);
                }
            });
            row = scratch_.bits();
        }
    }

    // -- Trimming, renumbering and deletion points. --------------------------

    void trim() {
        reached_.assign(derivations_.size(), 0);
        input_used_.assign(num_input_clauses_, 0);

        std::vector<std::size_t> stack{empty_derivation_};
        reached_[empty_derivation_] = 1;
        while (!stack.empty()) {
            const std::size_t index = stack.back();
            stack.pop_back();
            for (const NodeId node : derivations_[index].reasons) {
                const std::size_t next = derivation_of(node);
                if (next == no_index) {
                    input_used_[node] = 1;
                } else if (!reached_[next]) {
                    reached_[next] = 1;
                    stack.push_back(next);
                }
            }
        }

        // Emission follows derivation order, so ids and last uses follow from it.
        proof_ids_.assign(derivations_.size(), 0);
        std::size_t next_id = num_input_clauses_ + 1;
        for (std::size_t index = 0; index < derivations_.size(); ++index) {
            if (reached_[index]) {
                proof_ids_[index] = next_id++;
            }
        }

        std::vector<std::size_t> last_input_use(num_input_clauses_, no_index);
        std::vector<std::size_t> last_use(derivations_.size(), no_index);
        for (std::size_t index = 0; index < derivations_.size(); ++index) {
            if (!reached_[index]) {
                continue;
            }
            for (const NodeId node : derivations_[index].reasons) {
                const std::size_t used = derivation_of(node);
                (used == no_index ? last_input_use[node] : last_use[used]) = index;
            }
        }

        // The deletions of each line, packed: every kept clause is in one line's.
        deletion_starts_.assign(derivations_.size() + 1, 0);
        for (const std::size_t use : last_input_use) {
            if (use != no_index) {
                ++deletion_starts_[use + 1];
            }
        }
        for (const std::size_t use : last_use) {
            if (use != no_index) {
                ++deletion_starts_[use + 1];
            }
        }
        for (std::size_t index = 1; index < deletion_starts_.size(); ++index) {
            deletion_starts_[index] += deletion_starts_[index - 1];
        }
        std::vector<std::size_t> cursors(deletion_starts_.begin(), deletion_starts_.end() - 1);
        deletion_ids_.resize(deletion_starts_.back());
        for (std::size_t id = 0; id < num_input_clauses_; ++id) {
            if (last_input_use[id] != no_index) {
                deletion_ids_[cursors[last_input_use[id]]++] = id + 1;
            }
        }
        for (std::size_t index = 0; index < derivations_.size(); ++index) {
            if (last_use[index] != no_index) {
                deletion_ids_[cursors[last_use[index]]++] = proof_ids_[index];
            }
        }
    }

    // -- Pass 2: write the lines the proof keeps. ----------------------------

    void emit(RawRecordSource& source, std::ostream& out) {
        std::vector<std::size_t> unused;
        for (std::size_t id = 0; id < num_input_clauses_; ++id) {
            if (!input_used_[id]) {
                unused.push_back(id + 1);
            }
        }
        if (!unused.empty()) {
            write_deletions(out, unused);
        }

        source.rewind();
        RawRecord record;
        std::size_t unit_index = 0;
        while (source.next(record)) {
            if (const auto* learned = std::get_if<RawLearned>(&record)) {
                summary_.kept_learned +=
                    emit_line(out, clauses_[learned->id].derivation, learned->rows);
            } else if (const auto* compaction = std::get_if<RawCompaction>(&record)) {
                for (std::size_t i = 0; i < compaction->units.size(); ++i, ++unit_index) {
                    const std::size_t index = units_[unit_index].derivation;
                    if (index == no_index) {
                        continue;
                    }
                    summary_.unit_lines +=
                        emit_line(out, index, {&derivations_[index].row, 1});
                }
            } else if (const auto* empty = std::get_if<RawEmpty>(&record)) {
                summary_.kept_learned += emit_line(out, clauses_[empty->id].derivation, {});
            }
        }
    }

    /// @return Whether the line was kept, and so written.
    bool emit_line(std::ostream& out, std::size_t index, std::span<const RawRow> rows) {
        if (index == no_index || !reached_[index]) {
            return false;
        }
        out << proof_ids_[index];
        for (const RawRow& row : rows) {
            out << ' ';
            write_token(out, row);
        }
        out << " 0";
        for (const NodeId node : derivations_[index].reasons) {
            out << ' ' << proof_id_of(node);
        }
        out << " 0\n";
        const std::span<const std::size_t> deletions(
            deletion_ids_.data() + deletion_starts_[index],
            deletion_starts_[index + 1] - deletion_starts_[index]);
        if (!deletions.empty()) {
            write_deletions(out, deletions);
        }
        return true;
    }

    void write_deletions(std::ostream& out, std::span<const std::size_t> ids) {
        const char* separator = "";
        for (const std::size_t id : ids) {
            out << separator << id;
            separator = " ";
        }
        out << " 0\n";
        ++summary_.deletion_lines;
    }

    /// @brief A row as `-` for a set constant bit then the 1-based variables.
    void write_token(std::ostream& out, const RawRow& row) const {
        const bool constant = !row.empty() && row.back() == num_vars_;
        if (constant) {
            out << '-';
        }
        const std::size_t variables = row.size() - (constant ? 1u : 0u);
        for (std::size_t i = 0; i < variables; ++i) {
            if (i != 0) {
                out << '+';
            }
            out << (row[i] + 1);
        }
    }

    // -- Small helpers. ------------------------------------------------------

    std::size_t add_derivation(NodeId node, RawRow row, std::vector<NodeId> reasons) {
        derivations_.push_back(Derivation{node, std::move(row), std::move(reasons)});
        return derivations_.size() - 1;
    }

    ClauseCache& clause(ClauseId id) {
        if (id >= unit_node_base) {
            throw std::runtime_error("the raw proof's clause ids are out of range");
        }
        if (id >= clauses_.size()) {
            clauses_.resize(id + 1);
        }
        return clauses_[id];
    }

    /// @return The line proving `node`, or `no_index` for an input clause.
    std::size_t derivation_of(NodeId node) const {
        return node >= unit_node_base ? units_[node - unit_node_base].derivation
                                      : clauses_[node].derivation;
    }

    std::size_t proof_id_of(NodeId node) const {
        const std::size_t index = derivation_of(node);
        return index == no_index ? node + 1 : proof_ids_[index];
    }

    /// @brief A row's linear negation: the constant bit flipped.
    RawRow negate(const RawRow& row) const {
        RawRow negated = row;
        if (!negated.empty() && negated.back() == num_vars_) {
            negated.pop_back();
        } else {
            negated.push_back(num_vars_);
        }
        return negated;
    }

    std::size_t num_vars_;
    std::size_t num_input_clauses_;
    EquationRepr repr_;

    std::vector<RowSpace> epochs_;      ///< One echelon of eliminated units per compaction.
    std::vector<std::size_t> unit_of_col_; ///< Pivot column to unit; columns are eliminated once.
    std::vector<Unit> units_;
    std::vector<ClauseCache> clauses_;
    std::vector<Derivation> derivations_; ///< In emission order.
    std::size_t empty_derivation_ = no_index;

    std::vector<std::size_t> unit_stamps_; ///< Per-reduction deduplication of units.
    std::size_t unit_stamp_ = 0;
    std::vector<std::size_t> line_stamps_; ///< Per-line deduplication of insertions.
    std::size_t line_stamp_ = 0;

    std::vector<char> reached_;
    std::vector<char> input_used_;
    std::vector<std::size_t> proof_ids_;
    std::vector<std::size_t> deletion_starts_; ///< Into `deletion_ids_`, one per line plus an end.
    std::vector<std::size_t> deletion_ids_;

    Equation scratch_;
    PostprocessSummary summary_;
};

} // namespace

PostprocessSummary postprocess_raw_proof(RawRecordSource& source, std::size_t num_vars,
                                         std::size_t num_input_clauses, EquationRepr repr,
                                         std::ostream& out,
                                         std::span<const std::vector<RawRow>> input_clause_rows) {
    Postprocessor postprocessor(num_vars, num_input_clauses, repr, input_clause_rows);
    return postprocessor.run(source, out);
}

} // namespace xorcle
