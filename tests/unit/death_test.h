#pragma once

/// @file death_test.h
/// @brief Watching an assert fire, in a child process, where asserts are on.

#if !defined(NDEBUG) && (defined(__APPLE__) || defined(__unix__))
#define XORCLE_HAS_DEATH_TESTS 1

#include <csignal>
#include <cstdio>

#include <sys/wait.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif

namespace xorcle {

/// @brief Whether @p body dies of a signal when run in a child process.
///
/// The child inherits doctest's crash handler, which would report the expected
/// abort as a failure, so it restores the default one and silences stderr.
template <class Body>
bool dies_in_child(Body body) {
    const pid_t child = ::fork();
    if (child < 0) {
        return false;
    }
    if (child == 0) {
#ifdef __linux__
        // Not dumpable, so the abort skips the system's core-dump handler.
        ::prctl(PR_SET_DUMPABLE, 0);
#endif
        std::signal(SIGABRT, SIG_DFL);
        std::freopen("/dev/null", "w", stderr);
        body();
        ::_exit(0);
    }
    int status = 0;
    return ::waitpid(child, &status, 0) == child && WIFSIGNALED(status);
}

} // namespace xorcle

#endif
