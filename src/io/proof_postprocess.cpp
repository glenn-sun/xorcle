#include "io/proof_postprocess.h"

#include <algorithm>
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

/// @brief Above every id a raw proof can hold, so a corrupt one fails rather
/// than allocating.
constexpr ClauseId max_clause_id = std::size_t{1} << 48;

/// @brief One line the proof may keep.
///
/// A reason is a reference: an input clause's 0-based id below
/// `num_input_clauses`, and a derivation's index plus `num_input_clauses` above.
struct Derivation {
    std::vector<std::size_t> reasons;
    std::vector<RawRow> rows; ///< A rewrite line's rows; a raw line's are read back.
    bool rewrite = false;
};

/// @brief A clause as the proof currently cites it.
struct ClauseCache {
    std::vector<RawRow> rows;          ///< What `current` proves.
    std::size_t derivation = no_index; ///< The raw line's own.
    std::size_t current = no_index;    ///< The reference a chain cites the clause by.
    /// How many root units there were when `current` was proved.
    std::size_t watermark = 0;
    bool known = false;
};

/// @brief A root unit of the cumulative echelon.
struct RootUnit {
    std::size_t line;
    std::size_t compaction; ///< The 0-based index of the compaction record holding it.
};

/// @brief Turns a raw proof into an LRUP(⊕) proof in two passes over the source.
class Postprocessor {
public:
    Postprocessor(std::size_t num_vars, std::size_t num_input_clauses, EquationRepr repr,
                  std::span<const std::vector<RawRow>> input_clause_rows)
        : num_vars_(num_vars),
          num_input_clauses_(num_input_clauses),
          repr_(repr),
          unit_echelon_(repr, num_vars),
          col_idx_to_unit_idx_(num_vars + 1, no_index),
          unit_stamps_(num_vars + 1, 0),
          scratch_(make_equation(repr, num_vars)) {
        assert(input_clause_rows.size() == num_input_clauses &&
               "postprocess_raw_proof: one row list per input clause");
        clauses_.resize(num_input_clauses);
        for (std::size_t id = 0; id < num_input_clauses; ++id) {
            clauses_[id].rows = input_clause_rows[id];
            clauses_[id].current = id;
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
    // -- Pass 1: every final reason list. ------------------------------------

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
        std::vector<std::size_t> reasons = final_reasons(learned.reasons);
        ClauseCache& cache = clause(learned.id);
        cache.rows = learned.rows;
        cache.watermark = unit_echelon_.rows();
        cache.known = true;
        cache.derivation = add_derivation(std::move(reasons));
        cache.current = num_input_clauses_ + cache.derivation;
    }

    void scan_empty(const RawEmpty& empty) {
        if (empty_derivation_ != no_index) {
            throw std::runtime_error("the raw proof has more than one empty clause");
        }
        ++summary_.raw_learned;
        std::vector<std::size_t> reasons = final_reasons(empty.reasons);
        ClauseCache& cache = clause(empty.id);
        cache.known = true;
        cache.derivation = add_derivation(std::move(reasons));
        cache.current = num_input_clauses_ + cache.derivation;
        empty_derivation_ = cache.derivation;
    }

    /// @brief The reasons a line needs: each raw reason's current line, after
    /// proving it again if root units since that line reduce it.
    std::vector<std::size_t> final_reasons(const std::vector<ClauseId>& raw_reasons) {
        std::vector<std::size_t> reasons;
        for (const ClauseId id : raw_reasons) {
            ClauseCache& cache = clause(id);
            if (!cache.known) {
                throw std::runtime_error("the raw proof names an unknown reason clause");
            }
            if (unit_echelon_.rows() > cache.watermark) {
                reduce_by_units(cache);
            }
            reasons.push_back(cache.current);
        }
        return reasons;
    }

    /// @brief Add a line proving `cache`'s rows as the units on lines `units`
    /// have reduced them, from those lines and then the clause as last proved,
    /// and cite it from now on.
    ///
    /// The units' lines prove the compactions' echelon rows, so each propagates
    /// past rows already reduced by all of them.
    void rewrite(ClauseCache& cache, std::vector<std::size_t> units) {
        Derivation derivation;
        derivation.rewrite = true;
        for (RawRow& row : cache.rows) {
            if (row.empty()) {
                continue;
            }
            assert(!(row.size() == 1 && row[0] == num_vars_) &&
                   "a clause the raw proof still names is not satisfied by root units");
            derivation.rows.push_back(std::move(row));
        }
        cache.rows = derivation.rows;
        derivation.reasons = std::move(units);
        derivation.reasons.push_back(cache.current);
        cache.current = num_input_clauses_ + add_derivation(std::move(derivation));
    }

    /// @brief Append a compaction's units to the cumulative echelon, mapping
    /// each pivot to its unit.
    ///
    /// A unit holds no pivot of an earlier compaction's units, which that
    /// compaction eliminated, so inserting it changes no stored row.
    void scan_compaction(const RawCompaction& compaction) {
        const std::size_t index = num_compactions_++;
        for (const RawUnit& unit : compaction.units) {
            const std::size_t unit_derivation = clause(unit.id).derivation;
            if (unit_derivation == no_index && unit.id >= num_input_clauses_) {
                throw std::runtime_error("the raw proof names an unknown root unit line");
            }
            Equation unit_row = make_equation(repr_, num_vars_);
            unit_row.assign_bits(unit.row);
            Equation probe = unit_row;
            bool holds_eliminated = false;
            unit_echelon_.reduce_get_coefficients(probe).for_each_set([&](std::size_t row_idx) {
                if (units_[unit_of(row_idx)].compaction != index) {
                    holds_eliminated = true;
                }
            });
            if (holds_eliminated) {
                throw std::runtime_error(
                    "a root unit holds a variable an earlier compaction eliminated");
            }
            // Stored unchanged, the row is exactly what the unit's line proves,
            // so its pivot can stand for that line.
            const std::optional<std::size_t> row_idx =
                unit_echelon_.add_row_get_row_idx(std::move(unit_row));
            if (!row_idx || unit_echelon_.row(*row_idx).bits() != unit.row) {
                throw std::runtime_error("a root unit is not reduced by the ones before it");
            }
            const std::optional<std::size_t> pivot = unit_echelon_.row(*row_idx).leading_bit();
            assert(pivot && "a stored echelon row is never zero");
            col_idx_to_unit_idx_[*pivot] = units_.size();
            units_.push_back(RootUnit{
                unit_derivation == no_index ? unit.id : num_input_clauses_ + unit_derivation,
                index});
        }
    }

    /// @brief Reduce a clause about to be cited by every root unit, and prove
    /// it again if any was used.
    ///
    /// The clause's rows hold no pivot of the units before its watermark, so
    /// only units from there on take part. Their lines are cited compaction by
    /// compaction, each in the order its rows first use them.
    void reduce_by_units(ClauseCache& cache) {
        ++unit_stamp_;
        std::vector<std::size_t> used;
        for (RawRow& row : cache.rows) {
            scratch_.assign_bits(row);
            const IndexSet coefficients = unit_echelon_.reduce_get_coefficients(scratch_);
            if (coefficients.none()) {
                continue;
            }
            coefficients.for_each_set([&](std::size_t row_idx) {
                const std::size_t unit = unit_of(row_idx);
                if (unit < cache.watermark) {
                    throw std::runtime_error(
                        "the raw proof learns a clause over a variable compaction eliminated");
                }
                if (unit_stamps_[unit] != unit_stamp_) {
                    unit_stamps_[unit] = unit_stamp_;
                    used.push_back(unit);
                }
            });
            row = scratch_.bits();
        }
        cache.watermark = unit_echelon_.rows();
        if (used.empty()) {
            return;
        }
        const RawRow constant{num_vars_};
        if (std::find(cache.rows.begin(), cache.rows.end(), constant) != cache.rows.end()) {
            throw std::runtime_error("the raw proof names a reason clause root units satisfy");
        }
        std::stable_sort(used.begin(), used.end(), [&](std::size_t a, std::size_t b) {
            return units_[a].compaction < units_[b].compaction;
        });
        std::vector<std::size_t> lines;
        lines.reserve(used.size());
        for (const std::size_t unit : used) {
            lines.push_back(units_[unit].line);
        }
        rewrite(cache, std::move(lines));
    }

    std::size_t unit_of(std::size_t row_idx) const {
        const std::optional<std::size_t> pivot = unit_echelon_.row(row_idx).leading_bit();
        assert(pivot && "a stored echelon row is never zero");
        return col_idx_to_unit_idx_[*pivot];
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
            for (const std::size_t reference : derivations_[index].reasons) {
                if (reference < num_input_clauses_) {
                    input_used_[reference] = 1;
                    continue;
                }
                const std::size_t next = reference - num_input_clauses_;
                if (!reached_[next]) {
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
            for (const std::size_t reference : derivations_[index].reasons) {
                (reference < num_input_clauses_
                     ? last_input_use[reference]
                     : last_use[reference - num_input_clauses_]) = index;
            }
        }

        // The deletions of each line, packed: every kept clause is in one line's deletions.
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
        std::size_t next_index = 0;
        while (source.next(record)) {
            if (const auto* learned = std::get_if<RawLearned>(&record)) {
                const std::size_t index = clauses_[learned->id].derivation;
                emit_rewrites(out, next_index, index);
                summary_.kept_learned += emit_line(out, index, learned->rows);
                next_index = index + 1;
            } else if (const auto* empty = std::get_if<RawEmpty>(&record)) {
                const std::size_t index = clauses_[empty->id].derivation;
                emit_rewrites(out, next_index, index);
                summary_.kept_learned += emit_line(out, index, {});
                next_index = index + 1;
            }
        }
    }

    /// @brief Write the rewrite lines among derivations `begin .. end - 1`,
    /// which are all rewrite lines.
    void emit_rewrites(std::ostream& out, std::size_t begin, std::size_t end) {
        for (std::size_t index = begin; index < end; ++index) {
            const Derivation& derivation = derivations_[index];
            assert(derivation.rewrite && "only rewrite lines come between raw lines");
            if (emit_line(out, index, derivation.rows)) {
                ++summary_.rewrite_lines;
                summary_.rewrite_unit_ids += derivation.reasons.size() - 1;
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
        for (const std::size_t reference : derivations_[index].reasons) {
            out << ' ' << proof_id_of(reference);
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

    std::size_t add_derivation(std::vector<std::size_t> reasons) {
        Derivation derivation;
        derivation.reasons = std::move(reasons);
        return add_derivation(std::move(derivation));
    }

    std::size_t add_derivation(Derivation derivation) {
        derivations_.push_back(std::move(derivation));
        return derivations_.size() - 1;
    }

    ClauseCache& clause(ClauseId id) {
        if (id >= max_clause_id) {
            throw std::runtime_error("the raw proof's clause ids are out of range");
        }
        if (id >= clauses_.size()) {
            clauses_.resize(id + 1);
        }
        return clauses_[id];
    }

    std::size_t proof_id_of(std::size_t reference) const {
        return reference < num_input_clauses_ ? reference + 1
                                              : proof_ids_[reference - num_input_clauses_];
    }

    std::size_t num_vars_;
    std::size_t num_input_clauses_;
    EquationRepr repr_;

    RowSpace unit_echelon_;                ///< Every compaction's units, in order.
    std::vector<std::size_t> col_idx_to_unit_idx_; ///< Pivot column to `units_` index.
    std::vector<RootUnit> units_;          ///< In insertion order.
    std::size_t num_compactions_ = 0;
    std::vector<ClauseCache> clauses_;
    std::vector<Derivation> derivations_; ///< In emission order.
    std::size_t empty_derivation_ = no_index;

    std::vector<std::size_t> unit_stamps_; ///< Per-citation deduplication of units.
    std::size_t unit_stamp_ = 0;

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
