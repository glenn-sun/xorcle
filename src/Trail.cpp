#include "Trail.h"

void TrailSnapshot::learn(const Equation& eqn, std::optional<std::reference_wrapper<Clause>> reason) {
    std::size_t inserted_index;
    span.add_row(eqn, &inserted_index);
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