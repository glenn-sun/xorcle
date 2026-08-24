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
    std::size_t num_vars = 0;

    /// @brief The clauses, in file order, each in its linear negation.
    std::vector<Clause> clauses;
};

/// @brief Parse the extended DIMACS/XNF format, as `README.md` documents it,
/// from an input stream.
///
/// Rows are echelonized on insertion, so a line with duplicate or linearly
/// dependent literals yields fewer rows than tokens, in row-echelon order rather
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
