#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "algebra/RowSpace.h"

namespace io {

/// @brief Parsed representation of a DIMACS file.
struct ParsedDimacs {
    std::size_t num_vars;                 ///< Number of variables in the file header.
    std::vector<algebra::Clause> clauses; ///< Parsed clause list.
};

/// @brief Parse a DIMACS file into clause structures.
/// @param filename Path to the DIMACS file.
/// @return ParsedDimacs containing variables and clauses.
/// @throws std::runtime_error on malformed input.
ParsedDimacs parse_dimacs_file(const std::string& filename);

} // namespace io
