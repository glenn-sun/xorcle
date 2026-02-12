#include "search/Trail.h"

namespace search {
void TrailSnapshot::learn(const algebra::Equation& eqn, std::optional<heuristics::ClauseId> reason) {
    std::optional<std::size_t> maybe_inserted_index = span.add_row_get_index(eqn);
    if (maybe_inserted_index) {
        std::size_t inserted_index = *maybe_inserted_index;
        reasons.push_back(reason);

        // Update order map
        for (std::size_t& span_index : order_map) {
            if (span_index >= inserted_index) {
                ++span_index;
            }
        }
        order_map.push_back(inserted_index);

        // Update reverse order map
        reverse_order_map.insert(reverse_order_map.begin() + inserted_index, order_map.size() - 1);
    }
}
} // namespace search
