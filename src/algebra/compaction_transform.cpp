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
    out.dropped_columns_ = dropped_columns;
    out.old_to_new_.assign(old_num_vars, npos);
    out.new_to_old_.reserve(old_num_vars);

    for (std::size_t old_index = 0; old_index < old_num_vars; ++old_index) {
        if (dropped_columns.test(old_index)) {
            continue;
        }
        out.old_to_new_[old_index] = out.new_to_old_.size();
        out.new_to_old_.push_back(old_index);
    }
    out.new_num_vars_ = out.new_to_old_.size();

    // A postcondition, not a precondition: the two maps this builds must be
    // mutual inverses on the surviving variables.
#ifndef NDEBUG
    for (std::size_t old_index = 0; old_index < old_num_vars; ++old_index) {
        const std::size_t mapped = out.old_to_new_[old_index];
        if (mapped == npos) {
            continue;
        }
        assert(mapped < out.new_num_vars_ && out.new_to_old_[mapped] == old_index &&
               "compaction mapping is not invertible");
    }
#endif
    return out;
}

} // namespace xorcle
