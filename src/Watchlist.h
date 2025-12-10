#include "bit/vector.h"

using ClauseId = std::size_t;
using EquationId = std::size_t;

class Watchlist {
public:
    std::vector<std::vector<ClauseId>> by_leading_term;
    Watchlist(std::size_t num_vars) : by_leading_term(num_vars) {}
};