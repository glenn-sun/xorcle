#pragma once

#include <cstddef>

namespace xorcle {

/// @brief Count of global `operator new` calls since the program started.
///
/// The test binary replaces the global allocation functions so that
/// `assign_from()`'s freedom from allocation -- the whole point of the
/// preallocated-scratch discipline -- can be asserted rather than assumed.
std::size_t allocation_count() noexcept;

} // namespace xorcle
