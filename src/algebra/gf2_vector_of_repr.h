#pragma once

#include <cassert>
#include <type_traits>
#include <utility>

#include "algebra/dense/dense_gf2_vector.h"
#include "algebra/equation_repr.h"
#include "algebra/gf2_vector.h"
#include "algebra/sparse/sparse_gf2_vector.h"

namespace xorcle {

/// @brief Call `f(std::type_identity<V>{})` with the `GF2Vector` type `V` that
/// implements `repr`, and return what it returns: the one place a representation
/// is mapped to its type.
template <class F>
decltype(auto) with_gf2_vector_of(EquationRepr repr, F&& f) {
    switch (repr) {
        case EquationRepr::dense:
            return std::forward<F>(f)(std::type_identity<DenseGF2Vector>{});
        case EquationRepr::sparse:
            return std::forward<F>(f)(std::type_identity<SparseGF2Vector>{});
    }
    assert(false && "with_gf2_vector_of: not a representation");
    std::unreachable();
}

} // namespace xorcle
