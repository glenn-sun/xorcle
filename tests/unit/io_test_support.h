#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "algebra/equation.h"
#include "algebra/equation_repr.h"
#include "algebra/row_echelon_matrix.h"
#include "io/dimacs.h"
#include "io/proof_writer.h"

namespace xorcle {

/// @brief A path in the temp directory, unique to this process and @p name.
std::filesystem::path scratch_path(std::string_view name);

/// @brief Each clause's rows, as `LrupChecker` takes them.
std::vector<std::vector<ProofRow>> clause_rows(const std::vector<Clause>& clauses);

/// @brief Each clause's rows over the declared variables.
std::vector<std::vector<ProofRow>> clause_rows(const ParsedDimacs& parsed);

/// @brief The LRUP(⊕) checker of `tools/check_lrup_parity.rs`, over `RowSpace`.
///
/// Both bases reduce fully, so they agree on membership and on which residues
/// are equal -- all the checker ever asks. Like it, this one fails a proof that
/// never derives the empty clause, deletes an id that is not live, or learns an
/// id no greater than every earlier one, and passes a unit reason whose
/// equation is already known.
class LrupChecker {
public:
    /// @param inputs The input clauses' rows, which take ids 1 onwards.
    LrupChecker(EquationRepr repr, std::size_t num_vars,
                const std::vector<std::vector<ProofRow>>& inputs);

    /// @return An error message, empty when the proof checks out.
    std::string check(const std::string& text);

private:
    using Rows = std::vector<ProofRow>;
    enum class Status { conflict, deduced, already_known, none };

    std::string check_line(const std::vector<std::string>& tokens);
    Status propagate(const Rows& rows, RowSpace& basis) const;
    bool contains(const RowSpace& basis, const ProofRow& bits) const;
    Equation equation(const ProofRow& bits) const;
    ProofRow negate(const ProofRow& bits) const;
    ProofRow parse_token(const std::string& token) const;

    EquationRepr repr_;
    std::size_t num_vars_;
    std::unordered_map<std::size_t, Rows> clauses_;
    std::size_t max_id_;
    bool refuted_ = false;
};

} // namespace xorcle
