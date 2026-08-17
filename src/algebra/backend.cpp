#include "algebra/backend.h"

#include "algebra/dense/dense_backend.h"
#include "algebra/sparse/sparse_backend.h"

namespace xorcle {

// Only the backend factory headers are included here, and their signatures
// mention nothing but our own types -- that is what keeps gf2 confined to
// `algebra/dense/`.

std::unique_ptr<EquationStorage> make_equation_storage(Algebra backend, std::size_t num_vars) {
    switch (backend) {
        case Algebra::dense:
            return make_dense_equation_storage(num_vars);
        case Algebra::sparse:
            return make_sparse_equation_storage(num_vars);
    }
    return make_dense_equation_storage(num_vars);
}

std::unique_ptr<RowEchelonMatrixStorage> make_matrix_storage(Algebra backend,
                                                             std::size_t num_vars) {
    switch (backend) {
        case Algebra::dense:
            return make_dense_matrix_storage(num_vars);
        case Algebra::sparse:
            return make_sparse_matrix_storage(num_vars);
    }
    return make_dense_matrix_storage(num_vars);
}

Equation make_equation(Algebra backend, std::size_t num_vars) {
    return Equation(backend, num_vars);
}

RowEchelonMatrix make_matrix(Algebra backend, std::size_t num_vars) {
    return RowEchelonMatrix(backend, num_vars);
}

} // namespace xorcle
