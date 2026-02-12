#pragma once

#include <string>
#include <vector>

#include "algebra/RowSpace.h"

namespace io {
struct ParsedDimacs {
    std::size_t num_vars;
    std::vector<algebra::Clause> clauses;
};

ParsedDimacs parse_dimacs_file(const std::string& filename);
} // namespace io
