#include <rex/platform.h>

#if !REX_PLATFORM_PS5
#error This implementation is for PS5 only
#endif

#include <rex/thread.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <pthread.h>
#include <pthread_np.h>
#include <sched.h>
#include <string>
#include <string_view>
#include <time.h>

namespace rex::thread {

// The PS5 does not require a separate affinity
// configuration step before using its native APIs.
// This matches the existing POSIX implementation.
void EnableAffinityConfiguration() {
}

// Yield the native host thread.
void MaybeYield() {
    (void)::sched_yield();

    std::atomic_thread_fence(
        std::memory_order_seq_cst
    );
}

// Sleep for the requested duration, retrying when
// interrupted by a signal.
void Sleep(std::chrono::microseconds duration) {
    if (duration <= std::chrono::microseconds::zero()) {
        return;
    }

    constexpr int64_t kMicrosPerSecond = 1000000;

    const int64_t micros = duration.count();

    timespec requested{};
    requested.tv_sec =
        static_cast<time_t>(micros / kMicrosPerSecond);
    requested.tv_nsec =
        static_cast<long>(
            (micros % kMicrosPerSecond) * 1000
        );

    timespec remaining{};

    while (::nanosleep(&requested, &remaining) == -1) {
        if (errno != EINTR) {
            break;
        }

        requested = remaining;
    }
}

// FreeBSD-style thread naming, as exposed by
// the PS5 payload SDK.
void set_current_thread_name(std::string_view name) {
    // Use a conservative native thread-name length.
    std::string native_name(name.substr(0, 15));

    ::pthread_set_name_np(
        ::pthread_self(),
        native_name.c_str()
    );
}

} // namespace rex::thread
