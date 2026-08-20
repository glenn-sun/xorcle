#pragma once

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "algebra/equation.h"
#include "algebra/row_echelon_matrix.h"
#include "core/clause_id.h"

namespace xorcle {

/// @brief A proof file that cannot be written, or a run configuration that
/// cannot produce a proof.
///
/// User-facing, so an exception rather than an assert. Violations of the
/// logger's own preconditions are asserts instead.
class LrupError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// @brief The conventional proof file name, used when none is given.
inline constexpr std::string_view default_proof_path = "out.lrup";

/// @brief The message to report when a run asks for a proof with compaction on.
///
/// Proof emission with compaction is not supported, and the combination must
/// fail immediately rather than emit a wrong or incomplete proof. The message
/// names the fix.
inline constexpr std::string_view proof_requires_no_compaction_message =
    "--proof requires --compact=false: proof emission with compaction enabled is not "
    "supported in Xorcle 0.2";

/// @brief Reject the unsupported combination of proof emission and compaction.
///
/// Kept next to the logger so the rule lives with the thing it protects; wiring
/// it into option parsing is the CLI's job.
/// @throws LrupError With `proof_requires_no_compaction_message` if both are true.
void require_compaction_disabled_for_proof(bool proof_enabled, bool compaction_enabled);

/// @brief Writer for LRUP(⊕) proofs.
///
/// LRUP(⊕) is the parity-clause analogue of LRUP: each line carries a derived
/// clause *and* the ordered antecedent ids that derive it, so a checker replays
/// a propagation sequence instead of searching for one.
/// `tools/check_lrup_parity.py` is an independent checker and the normative
/// reader for this format.
///
/// ## Clause identifiers
///
/// Input clauses are never written; they take *implicit* ids counting from 1, in
/// the order `parse_dimacs()` returns them. Internal `ClauseId`s are 0-based, so
/// internal id `c` is proof id `c + 1`. The logger applies that offset itself,
/// to both the new id and every reason id: callers always pass 0-based ids.
///
/// ## Line format
///
/// ```
/// learned:  <new id> <token> <token> ... 0 <reason id> <reason id> ... 0
/// empty:    <new id> 0 <reason id> <reason id> ... 0
/// ```
///
/// A line with a single `0` is a deletion line in the checker's grammar; this
/// logger never writes one, so every line it emits contains exactly two.
///
/// Each token is one clause row in the syntax the parser reads: the row's
/// variables ascending, joined by `+`, prefixed by `-` when the constant column
/// is set. Parser, storage and logger round-trip exactly. Rows are emitted in
/// row-echelon order -- how the clause stores them, not how the input wrote them.
///
/// ## Reason order: the contract with the solver
///
/// **Reason ids are written in derivation order** -- the order a checker must
/// replay them in for each to propagate, with the last one conflicting. Conflict
/// analysis produces the opposite order, pushing the conflict clause first and
/// then each reason as it resolves backwards through the trail. So the solver
/// **reverses the collected list exactly once**, immediately before calling
/// `log_learned_clause()`. Reversing twice, or not at all, yields a proof the
/// checker rejects with "reason clause did not propagate". `log_empty_clause()`
/// takes the root trail's reasons in trail order, which is already derivation
/// order; no reversal there.
///
/// ## The fixed variable numbering
///
/// @pre The variable numbering never changes under the logger. Every clause logged
/// must be over exactly `num_vars()` variables -- the count from the input
/// header -- because the tokens and the input clause ids they refer to are only
/// meaningful in that one numbering. `log_learned_clause()` asserts it;
/// `log_empty_clause()` writes no tokens and so has nothing to check.
///
/// Compaction renumbers variables mid-solve and discards the root trail, which
/// breaks both halves of that precondition. Nothing here bridges that gap; use
/// `require_compaction_disabled_for_proof()` to reject the combination up front.
///
/// ## The temporary file
///
/// Lines go to a temporary file next to the final path, moved into place by
/// `finalize(true)`. A run that ends SAT, or dies before finalizing, leaves no
/// proof file behind rather than a truncated one.
class LrupLogger {
public:
    /// @brief Open a proof logger writing to a temporary file next to `final_path`.
    ///
    /// `num_vars` is fixed for the logger's lifetime.
    /// @throws LrupError If the temporary file cannot be opened.
    LrupLogger(std::filesystem::path final_path, std::size_t num_vars);

    /// @brief Delete the temporary file unless the proof was finalized.
    ~LrupLogger();

    LrupLogger(const LrupLogger&) = delete;
    LrupLogger& operator=(const LrupLogger&) = delete;

    std::size_t num_vars() const noexcept { return num_vars_; }

    /// @brief Write one learned-clause line.
    /// @param new_id Id of the learned clause, 0-based.
    /// @param clause The learned clause, in its linear negation.
    /// @param reason_ids Antecedent clause ids, 0-based, in derivation order.
    /// @pre `clause.num_vars() == num_vars()`, no row of `clause` is
    /// constant-only, and `finalize()` has not run.
    void log_learned_clause(ClauseId new_id, const Clause& clause,
                            const std::vector<ClauseId>& reason_ids);

    /// @brief Write the final empty-clause line.
    /// @param new_id Id of the empty clause, 0-based.
    /// @param reason_ids Antecedent clause ids, 0-based, in derivation order.
    /// @pre `finalize()` has not run.
    void log_empty_clause(ClauseId new_id, const std::vector<ClauseId>& reason_ids);

    /// @brief Close the proof, either keeping or discarding what was written.
    /// @param keep True to move the temporary file to the final path, false to
    /// delete it. The solver passes whether the run ended UNSAT: a proof of a
    /// satisfiable instance proves nothing and is not kept.
    /// @throws LrupError If the proof was to be kept and cannot be written.
    /// @note Calling it twice is a no-op the second time.
    void finalize(bool keep);

private:
    std::filesystem::path final_path_;
    std::filesystem::path temp_path_;
    std::ofstream out_;
    std::size_t num_vars_;
    bool finalized_ = false;

    /// @brief Write the trailing `0 <reason id> ... 0` of a line.
    void write_reasons(const std::vector<ClauseId>& reason_ids);

    void write_token(const Equation& row);
};

} // namespace xorcle
