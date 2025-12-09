#include "Equation.h"
#include <string>

std::string Equation::to_string(PrintMode mode) const {
    std::ostringstream oss;
    if_set_call([&](std::size_t i) {
        if (i != size() - 1) {
            if (oss.tellp() > 0) oss << " + ";
            oss << "x" << i;
        }
    });

    auto constant = back();
    if (mode.clause_mode == ClausePrintMode::as_clause) {
        constant = !constant;
    }
    switch (mode.equation_mode) {
        case EquationPrintMode::standard:
            if (oss.tellp() == 0) oss << "0";
            oss << " = " << (constant ? "1" : "0");
            break;
        case EquationPrintMode::equals_0:
            if (oss.tellp() == 0) {
                oss << (constant ? "1" : "0") << " = 0";
            } else {
                oss << (constant ? " + 1" : "") << " = 0";
            }
            break;
        case EquationPrintMode::equals_1:
            if (oss.tellp() == 0) {
                oss << (constant ? "0" : "1") << " = 1";
            } else {
                oss << (constant ? "" : " + 1") << " = 1";
            }
            break;
    }
    
    return "[" + oss.str() + "]";
}