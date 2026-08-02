#include "algebra/algebra.h"

namespace xorcle {

std::string_view to_string(Algebra backend) noexcept {
    switch (backend) {
        case Algebra::dense:
            return "dense";
        case Algebra::sparse:
            return "sparse";
    }
    return "dense";
}

std::optional<Algebra> algebra_from_string(std::string_view name) noexcept {
    if (name == "dense") {
        return Algebra::dense;
    }
    if (name == "sparse") {
        return Algebra::sparse;
    }
    return std::nullopt;
}

} // namespace xorcle
