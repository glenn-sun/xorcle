#pragma once

#include <cstddef>
#include <filesystem>
#include <iosfwd>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "algebra/equation_repr.h"
#include "algebra/row_echelon_matrix.h"

namespace xorcle {

/// @brief A malformed instance file.
class DimacsError : public std::runtime_error {
public:
    /// @brief Build the error message `"<origin>:<line>: <message>"`, with
    /// `line_number` 1-based, or `0` for a whole-input error.
    DimacsError(std::string_view origin, std::size_t line_number, std::string_view message);
};

struct ParsedDimacs {
    /// @brief The variable count the header declares.
    std::size_t num_vars = 0;

    /// @brief The variables some clause uses, ascending.
    std::vector<std::size_t> used_vars;

    /// @brief The clauses, in file order, each in its linear negation, over
    /// `used_vars` alone: bit `i` is variable `used_vars[i]`.
    std::vector<Clause> clauses;
};

/// @brief Parse the extended DIMACS/XNF format, CryptoMiniSat's CNF-XOR lines
/// included, as `README.md` documents it, from an input stream.
///
/// A clause line whose first non-blank character is `x` is a CNF-XOR XOR, parsed
/// into a clause of one equation; nothing else tells the formats apart, so they mix
/// freely. The header's clause count may either include or exclude the XOR lines.
///
/// Rows are echelonized on insertion, so a line with duplicate or linearly
/// dependent equations yields fewer rows than tokens, in row-echelon order rather
/// than source order.
/// @throws DimacsError On any malformed input.
ParsedDimacs parse_dimacs(std::istream& input, EquationRepr repr,
                          std::string_view origin = "<input>");

/// @throws DimacsError On any malformed input.
ParsedDimacs parse_dimacs_string(std::string_view text, EquationRepr repr,
                                 std::string_view origin = "<string>");

/// @throws DimacsError If the file cannot be opened, or on any malformed input.
ParsedDimacs parse_dimacs_file(const std::filesystem::path& path, EquationRepr repr);

} // namespace xorcle
