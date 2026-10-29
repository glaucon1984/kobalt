// libstdc++ 12's futex-based waiter for std::future, missing from Debian's arm-linux-gnueabi
// archive: that libstdc++ was built for ARMv5 (atomics not lock-free), so <future> there uses a
// mutex and condition variable and futex.o defines nothing. Compiled for ARMv7 against the same
// headers, Kobalt's std::future (fonts.cpp prepares the interface fonts with std::async) expects
// std::__atomic_futex_unsigned_base and the linker otherwise falls back to the sysroot's shared
// libstdc++ 6.3 ("DSO missing from command line"). These are the three methods of
// libstdc++-v3/src/c++11/futex.cc, on the raw futex syscall.
#include <future>
#include <chrono>
#include <cerrno>
#include <climits>
#include <ctime>
#include <sys/syscall.h>
#include <sys/time.h>
#include <unistd.h>

namespace {
const int futexWaitPrivate = 0 | 128;
const int futexWakePrivate = 1 | 128;

bool waitRelative(unsigned *address, unsigned value, const struct timespec *relative) {
    if (syscall(SYS_futex, address, futexWaitPrivate, value, relative) == -1 && errno == ETIMEDOUT)
        return false;
    return true;
}

bool waitUntil(unsigned *address, unsigned value, bool hasTimeout, long long nowSeconds, long nowNanoseconds,
               std::chrono::seconds seconds, std::chrono::nanoseconds nanoseconds) {
    if (!hasTimeout)
        return waitRelative(address, value, nullptr);
    struct timespec relative;
    relative.tv_sec = static_cast<time_t>(seconds.count() - nowSeconds);
    relative.tv_nsec = static_cast<long>(nanoseconds.count() - nowNanoseconds);
    if (relative.tv_nsec < 0) {
        relative.tv_nsec += 1000000000L;
        --relative.tv_sec;
    }
    if (relative.tv_sec < 0)
        return false;
    return waitRelative(address, value, &relative);
}
}

namespace std {
bool __atomic_futex_unsigned_base::_M_futex_wait_until(unsigned *__addr, unsigned __val, bool __has_timeout,
                                                       chrono::seconds __s, chrono::nanoseconds __ns) {
    struct timeval now;
    gettimeofday(&now, nullptr);
    return waitUntil(__addr, __val, __has_timeout, now.tv_sec, now.tv_usec * 1000L, __s, __ns);
}

bool __atomic_futex_unsigned_base::_M_futex_wait_until_steady(unsigned *__addr, unsigned __val, bool __has_timeout,
                                                              chrono::seconds __s, chrono::nanoseconds __ns) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return waitUntil(__addr, __val, __has_timeout, now.tv_sec, now.tv_nsec, __s, __ns);
}

void __atomic_futex_unsigned_base::_M_futex_notify_all(unsigned *__addr) {
    syscall(SYS_futex, __addr, futexWakePrivate, INT_MAX, nullptr);
}
}
