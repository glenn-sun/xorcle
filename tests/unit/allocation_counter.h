#pragma once

#include <cstddef>

namespace xorcle {

/// @brief Count of global `operator new` calls since the program started.
///
/// The test binary replaces the global allocation functions to keep this count.
std::size_t allocation_count() noexcept;

} // namespace xorcle
