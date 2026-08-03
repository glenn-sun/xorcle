#pragma once

#include <cstddef>
#include <filesystem>
#include <iosfwd>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "algebra/algebra.h"
#include "algebra/row_echelon_matrix.h"

namespace xorcle {

/// @brief A malformed instance file.
///
/// Parsing is where user-facing input errors live, so every rejection is an
/// exception rather than an assert or an error return.
/// `main` catches these, reports them, and exits non-zero.
class DimacsError : public std::runtime_error {
public:
    /// @brief Build the error message `"<origin>:<line>: <message>"`.
    /// @param origin Name of the input, e.g. a file path.
    /// @param line_number 1-based line the error was found on, or `0` if the
    /// error is about the input as a whole.
    /// @param message What was wrong.
    DimacsError(std::string_view origin, std::size_t line_number, std::string_view message);

    /// @brief The 1-based line the error was found on, or `0` for a whole-input error.
    std::size_t line_number() const noexcept { return line_number_; }

private:
    std::size_t line_number_;
};

/// @brief One parsed instance: the header's variable count and the clauses.
struct ParsedDimacs {
    /// @brief Number of variables declared in the header.
    std::size_t num_vars = 0;

    /// @brief The clauses, in file order, each in its linear negation.
    ///
    /// Clause `i` here is `ClauseId{i}`, which the LRUP(⊕) proof format writes
    /// as the 1-based id `i + 1` (see `io/lrup_logger.h`).
    std::vector<Clause> clauses;
};

/// @brief Parse the extended DIMACS/XNF format from an input stream.
///
/// The grammar, as accepted:
/// - Leading blank lines and lines whose first non-blank character is `c` are
///   skipped. The first other line is the header.
/// - The header is exactly four whitespace-separated tokens,
///   `p <cnf|xnf> <num_vars> <num_clauses>`. Both format tokens are accepted and
///   mean the same thing; `cnf` is what `tools/xnf_lib.py` writes.
/// - Every later non-blank, non-comment line is one clause and must end with a
///   `0` terminator. Nothing may follow that terminator on the line.
/// - A clause is a sequence of literal tokens. A plain token `[-]<var>` is the
///   equation `x<var> = 1`, or `x<var> = 0` when negated. A token
///   `[-]<var>+<var>+...` is the parity equation over those variables, with the
///   same reading of the leading `-`. So `-` sets the right-hand side to 0, and
///   the *stored* row is the literal's linear negation.
/// - The clause count in the header must match the number of clause lines.
///
/// Clause rows are echelonized on insertion, so a line with duplicate or
/// linearly dependent literals yields fewer rows than tokens, and the rows are
/// stored in row-echelon order rather than source order.
///
/// This is stricter than the 0.1 parser in three places, each of which turned a
/// silent misparse into an error: integer tokens must be consumed in full
/// (`3junk` was read as `3`), a `+` may not be trailing (`1+2+` was accepted),
/// and tokens after a clause terminator are rejected (they were dropped).
///
/// @param input Stream to read; read to end of stream.
/// @param backend Storage representation to build the clauses in. Both backends
/// parse the same text into the same clauses.
/// @param origin Name to put in error messages, e.g. the file path.
/// @return The declared variable count and the parsed clauses.
/// @throws DimacsError On any malformed input.
ParsedDimacs parse_dimacs(std::istream& input, Algebra backend,
                          std::string_view origin = "<input>");

/// @brief Parse the extended DIMACS/XNF format from an in-memory string.
///
/// The convenience form of `parse_dimacs()`, so a test can state an instance as
/// a string literal instead of writing a file.
/// @param text The whole instance.
/// @param backend Storage representation to build the clauses in.
/// @param origin Name to put in error messages.
/// @return The declared variable count and the parsed clauses.
/// @throws DimacsError On any malformed input.
ParsedDimacs parse_dimacs_string(std::string_view text, Algebra backend,
                                 std::string_view origin = "<string>");

/// @brief Parse an extended DIMACS/XNF file.
///
/// The thin wrapper over `parse_dimacs()`: it opens the file and does nothing
/// else, so all of the parsing is reachable without touching the filesystem.
/// @param path File to read.
/// @param backend Storage representation to build the clauses in.
/// @return The declared variable count and the parsed clauses.
/// @throws DimacsError If the file cannot be opened, or on any malformed input.
ParsedDimacs parse_dimacs_file(const std::filesystem::path& path, Algebra backend);

} // namespace xorcle
