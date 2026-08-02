#include <cstdio>

#ifndef XORCLE_VERSION
#define XORCLE_VERSION "0.0.0"
#endif

/// @brief Placeholder entry point.
///
/// The real command line -- the X-macro option table, the parser, and the
/// generated help -- arrives in phase 3. Until then
/// this exists only so the `xorcle` executable target links.
int
main() {
    std::printf("xorcle %s\n", XORCLE_VERSION);
    return 0;
}
