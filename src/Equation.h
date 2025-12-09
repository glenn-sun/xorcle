#pragma once

#include "PrintMode.h"
#include "bit/vector.h"

/// @brief An affine equation over GF(2).
///
/// Represented as a bit::vector whose last component is the constant 
/// term. In other words, if there are 3 variables, the equation 
/// x1 + x3 = 1 is represented as [1, 0, 1, 1]. Note that the false
/// equation 1 = 0 has a representation in this form: when there are
/// 3 variables, it is [0, 0, 0, 1].
class Equation : public bit::vector<> {
public:
    Equation() {}
    Equation(const bit::vector<>& v) : bit::vector<>(v) {}
    std::string to_string(PrintMode mode = PrintMode()) const;
};