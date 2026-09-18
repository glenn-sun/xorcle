#include "algebra/compaction_transform.h"

#include <cassert>

namespace xorcle {

CompactionTransform CompactionTransform::from_dropped_columns(std::size_t old_num_vars,
                                                              const IndexSet& dropped_columns) {
    assert(dropped_columns.size() == old_num_vars + 1 &&
           "dropped_columns must be indexed by column, over old_num_vars + 1 columns");
    assert(!dropped_columns.test(old_num_vars) && "the constant column is never dropped");

    CompactionTransform out;
    out.old_num_vars_ = old_num_vars;
    out.old_col_idx_to_new_col_idx_.assign(old_num_vars, npos);
    out.new_col_idx_to_old_col_idx_.reserve(old_num_vars);

    for (std::size_t old_col_idx = 0; old_col_idx < old_num_vars; ++old_col_idx) {
        if (dropped_columns.test(old_col_idx)) {
            continue;
        }
        out.old_col_idx_to_new_col_idx_[old_col_idx] = out.new_col_idx_to_old_col_idx_.size();
        out.new_col_idx_to_old_col_idx_.push_back(old_col_idx);
    }
    out.new_num_vars_ = out.new_col_idx_to_old_col_idx_.size();

    // A postcondition, not a precondition: the two maps this builds must be
    // mutual inverses on the surviving variables.
#ifndef NDEBUG
    for (std::size_t old_col_idx = 0; old_col_idx < old_num_vars; ++old_col_idx) {
        const std::size_t mapped = out.old_col_idx_to_new_col_idx_[old_col_idx];
        if (mapped == npos) {
            continue;
        }
        assert(mapped < out.new_num_vars_ &&
               out.new_col_idx_to_old_col_idx_[mapped] == old_col_idx &&
               "compaction mapping is not invertible");
    }
#endif
    return out;
}

} // namespace xorcle
